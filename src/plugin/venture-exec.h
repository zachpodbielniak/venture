/*
 * venture-exec.h - Running an exec plugin's program
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * An exec plugin is a program beside its manifest, run as a subprocess
 * that reads one request line on its standard input and answers in the
 * JSON-lines protocol (venture-jsonl.h) on its standard output.
 *
 * The rules are the ones that made git safe in the forge runner, applied
 * to a program somebody else wrote:
 *
 *   - an argv array and never a shell, so nothing is word-split;
 *   - the executable is resolved with realpath() on every run and must sit
 *     under the plugin's own directory, compared against `root + "/"` so a
 *     sibling named `<root>-evil` does not pass;
 *   - the environment is cleared to PATH, LANG and HOME plus the names the
 *     manifest declares, so the server's own variables do not leak;
 *   - settings and secrets travel on standard input only -- never argv,
 *     which any user can read through /proc, and never the environment,
 *     which a child's children inherit;
 *   - one deadline covers the whole run, and standard output and standard
 *     error are capped in bytes.
 *
 * It is not a sandbox. The program runs as the server's user and can read
 * whatever that user can; the rules keep the server from handing it more
 * than it asked for, which is a different promise. docs/plugins.org says
 * so to operators.
 *
 * #VentureExecSpec is frozen data, and venture_exec_run() touches neither
 * the database nor the configuration, so a run may be made on any thread
 * -- which is what the feeds worker needs.
 */

#ifndef VENTURE_EXEC_H
#define VENTURE_EXEC_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_EXEC_DEFAULT_TIMEOUT_SECONDS:
 *
 * How long a run may take when the manifest does not say.
 */
#define VENTURE_EXEC_DEFAULT_TIMEOUT_SECONDS (60)

/**
 * VENTURE_EXEC_MAX_TIMEOUT_SECONDS:
 *
 * The longest deadline a manifest may ask for.
 */
#define VENTURE_EXEC_MAX_TIMEOUT_SECONDS (3600)

/**
 * VENTURE_EXEC_DEFAULT_MAX_STDOUT:
 *
 * Bytes of standard output accepted when the manifest does not say.
 */
#define VENTURE_EXEC_DEFAULT_MAX_STDOUT (16 * 1024 * 1024)

/**
 * VENTURE_EXEC_MAX_MAX_STDOUT:
 *
 * The largest standard-output cap a manifest may ask for.
 */
#define VENTURE_EXEC_MAX_MAX_STDOUT (256 * 1024 * 1024)

/**
 * VENTURE_EXEC_DEFAULT_MAX_STDERR:
 *
 * Bytes of standard error kept when the manifest does not say.
 */
#define VENTURE_EXEC_DEFAULT_MAX_STDERR (64 * 1024)

/**
 * VENTURE_EXEC_MAX_MAX_STDERR:
 *
 * The largest standard-error cap a manifest may ask for.
 */
#define VENTURE_EXEC_MAX_MAX_STDERR (4 * 1024 * 1024)

/**
 * VENTURE_EXEC_MAX_ARGS:
 *
 * The most arguments a manifest may give its program.
 */
#define VENTURE_EXEC_MAX_ARGS (64)

/**
 * VENTURE_EXEC_REDACTED:
 *
 * What a secret's value is replaced with wherever a run's output repeats it.
 */
#define VENTURE_EXEC_REDACTED "[redacted]"

/**
 * venture_exec_resolve_within:
 * @root: a directory
 * @path: a path, relative to @root or absolute
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves @path with realpath() -- following every symlink -- and
 * refuses it unless the result is strictly inside the real @root. The
 * comparison is against `root + "/"`, so `/srv/plug-evil/x` is not inside
 * `/srv/plug`.
 *
 * Returns: (transfer full) (nullable): the resolved path, or %NULL with
 *   %VENTURE_ERROR_PERMISSION_DENIED (outside) or %VENTURE_ERROR_NOT_FOUND
 */
gchar *
venture_exec_resolve_within(
	const gchar	 *root,
	const gchar	 *path,
	GError		**error
);

typedef struct _VentureExecSpec VentureExecSpec;

#define VENTURE_TYPE_EXEC_SPEC (venture_exec_spec_get_type())

GType venture_exec_spec_get_type(void) G_GNUC_CONST;

/**
 * venture_exec_spec_new:
 * @name: the plugin's name, used in errors
 * @root: the plugin's directory
 * @executable: the program, relative to @root or absolute
 * @error: (out) (optional): return location for a #GError
 *
 * Describes how to run one program. The executable must resolve inside
 * @root and be an executable regular file; both are checked again on every
 * run, because files change after load.
 *
 * Configure the spec before handing it to anything that runs it: once
 * shared, it is read-only and safe to use from any thread.
 *
 * Returns: (transfer full) (nullable): a spec, or %NULL on error
 */
VentureExecSpec *
venture_exec_spec_new(
	const gchar	 *name,
	const gchar	 *root,
	const gchar	 *executable,
	GError		**error
);

/**
 * venture_exec_spec_ref:
 * @self: a spec
 *
 * Returns: (transfer full): @self
 */
VentureExecSpec *
venture_exec_spec_ref(VentureExecSpec *self);

/**
 * venture_exec_spec_unref:
 * @self: (transfer full): a spec
 */
void
venture_exec_spec_unref(VentureExecSpec *self);

/**
 * venture_exec_spec_set_args:
 * @self: a spec
 * @args: (array zero-terminated=1) (nullable): arguments after the program
 *
 * The fixed arguments, from the manifest. They are public -- /proc shows
 * them to every user -- so nothing configured or secret belongs here.
 */
void
venture_exec_spec_set_args(
	VentureExecSpec		*self,
	const gchar *const	*args
);

/**
 * venture_exec_spec_pass_env:
 * @self: a spec
 * @name: an environment variable the program may see
 * @error: (out) (optional): return location for a #GError
 *
 * Passes one of the server's environment variables through, with the
 * value it has now; an unset variable stays unset. The name must look
 * like one (`[A-Za-z_][A-Za-z0-9_]*`).
 *
 * Returns: %TRUE on success
 */
gboolean
venture_exec_spec_pass_env(
	VentureExecSpec	 *self,
	const gchar	 *name,
	GError		**error
);

/**
 * venture_exec_spec_set_timeout:
 * @self: a spec
 * @seconds: the deadline for a whole run, 1 to
 *   %VENTURE_EXEC_MAX_TIMEOUT_SECONDS
 */
void
venture_exec_spec_set_timeout(
	VentureExecSpec	*self,
	guint		 seconds
);

/**
 * venture_exec_spec_set_timeout_ms:
 * @self: a spec
 * @milliseconds: the deadline for a whole run, in milliseconds
 *
 * The same, finer: tests and the feeds worker's own deadline use it.
 */
void
venture_exec_spec_set_timeout_ms(
	VentureExecSpec	*self,
	guint		 milliseconds
);

/**
 * venture_exec_spec_set_limits:
 * @self: a spec
 * @max_stdout: bytes of standard output accepted; more fails the run
 * @max_stderr: bytes of standard error kept; more is read and dropped
 *
 * Standard output past its cap fails the run, because a truncated batch
 * looks like a complete one. Standard error past its cap is discarded but
 * still drained, so a chatty program is not killed for logging.
 */
void
venture_exec_spec_set_limits(
	VentureExecSpec	*self,
	gsize		 max_stdout,
	gsize		 max_stderr
);

/**
 * venture_exec_spec_get_name:
 * @self: a spec
 *
 * Returns: (transfer none): the plugin's name
 */
const gchar *
venture_exec_spec_get_name(VentureExecSpec *self);

/**
 * venture_exec_spec_get_root:
 * @self: a spec
 *
 * Returns: (transfer none): the plugin's directory, resolved
 */
const gchar *
venture_exec_spec_get_root(VentureExecSpec *self);

/**
 * venture_exec_spec_get_executable:
 * @self: a spec
 *
 * Returns: (transfer none): the program, as resolved when the spec was made
 */
const gchar *
venture_exec_spec_get_executable(VentureExecSpec *self);

/**
 * venture_exec_spec_get_timeout_ms:
 * @self: a spec
 *
 * Returns: the deadline, in milliseconds
 */
guint
venture_exec_spec_get_timeout_ms(VentureExecSpec *self);

/**
 * venture_exec_spec_get_max_stdout:
 * @self: a spec
 *
 * Returns: the standard-output cap, in bytes
 */
gsize
venture_exec_spec_get_max_stdout(VentureExecSpec *self);

/**
 * venture_exec_spec_get_max_stderr:
 * @self: a spec
 *
 * Returns: the standard-error cap, in bytes
 */
gsize
venture_exec_spec_get_max_stderr(VentureExecSpec *self);

typedef struct _VentureExecResult VentureExecResult;

#define VENTURE_TYPE_EXEC_RESULT (venture_exec_result_get_type())

GType venture_exec_result_get_type(void) G_GNUC_CONST;

/**
 * venture_exec_run:
 * @spec: what to run
 * @request: (nullable): the request object; the run adds `protocol`
 * @secrets: (nullable): secrets for the program, sent as the request's
 *   `secrets` member and redacted from everything the run hands back
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Runs the program to completion and reads its answer. Blocks the calling
 * thread for at most the spec's deadline, iterating a private main context
 * -- never the thread's default one, so no unrelated source runs nested
 * inside it.
 *
 * The request is written to standard input as one JSON line followed by
 * end-of-file. Every line the program writes must be a protocol-1 message;
 * the first that is not fails the run.
 *
 * A run that completes returns a result even when the program exited
 * non-zero or reported an `error` message, because a partial batch may
 * still be worth reading; venture_exec_result_check() turns either into a
 * #GError. %NULL is returned only when the run did not complete: it could
 * not start (%VENTURE_ERROR_PLUGIN), ran past its deadline
 * (%VENTURE_ERROR_TIMEOUT), wrote past its cap or broke the protocol
 * (%VENTURE_ERROR_PLUGIN), or was cancelled (%G_IO_ERROR_CANCELLED). The
 * whole process group is killed in each of those cases.
 *
 * Every string the result carries -- messages, standard error, the error
 * message -- has each secret value of six bytes or more replaced with
 * %VENTURE_EXEC_REDACTED.
 *
 * Returns: (transfer full) (nullable): the result, or %NULL on error
 */
VentureExecResult *
venture_exec_run(
	VentureExecSpec	 *spec,
	JsonObject	 *request,
	JsonObject	 *secrets,
	GCancellable	 *cancellable,
	GError		**error
);

/**
 * venture_exec_result_ref:
 * @self: a result
 *
 * Returns: (transfer full): @self
 */
VentureExecResult *
venture_exec_result_ref(VentureExecResult *self);

/**
 * venture_exec_result_unref:
 * @self: (transfer full): a result
 */
void
venture_exec_result_unref(VentureExecResult *self);

/**
 * venture_exec_result_get_messages:
 * @self: a result
 *
 * Returns: (transfer none) (element-type VentureJsonlMessage): every
 *   message, in the order written
 */
GPtrArray *
venture_exec_result_get_messages(VentureExecResult *self);

/**
 * venture_exec_result_get_exit_status:
 * @self: a result
 *
 * Returns: the exit code, or minus the signal number if a signal ended it
 */
gint
venture_exec_result_get_exit_status(VentureExecResult *self);

/**
 * venture_exec_result_get_stderr:
 * @self: a result
 *
 * Returns: (transfer none): what the program wrote on standard error, up
 *   to its cap, redacted; never %NULL
 */
const gchar *
venture_exec_result_get_stderr(VentureExecResult *self);

/**
 * venture_exec_result_get_stderr_truncated:
 * @self: a result
 *
 * Returns: %TRUE if standard error passed its cap
 */
gboolean
venture_exec_result_get_stderr_truncated(VentureExecResult *self);

/**
 * venture_exec_result_get_stdout_bytes:
 * @self: a result
 *
 * Returns: how many bytes of standard output were read
 */
gsize
venture_exec_result_get_stdout_bytes(VentureExecResult *self);

/**
 * venture_exec_result_get_error_message:
 * @self: a result
 *
 * Returns: (transfer none) (nullable): the first `error` message's text,
 *   redacted, or %NULL if the program reported none
 */
const gchar *
venture_exec_result_get_error_message(VentureExecResult *self);

/**
 * venture_exec_result_get_retry_after:
 * @self: a result
 *
 * Returns: the first `error` message's `retry_after` in seconds, or -1
 */
gint64
venture_exec_result_get_retry_after(VentureExecResult *self);

/**
 * venture_exec_result_check:
 * @self: a result
 * @error: (out) (optional): return location for a #GError
 *
 * Judges a completed run: a non-zero exit or an `error` message fails it
 * with %VENTURE_ERROR_PLUGIN, naming the reason and the tail of standard
 * error.
 *
 * Returns: %TRUE if the run succeeded
 */
gboolean
venture_exec_result_check(
	VentureExecResult	 *self,
	GError			**error
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureExecSpec, venture_exec_spec_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureExecResult, venture_exec_result_unref)

G_END_DECLS

#endif /* VENTURE_EXEC_H */
