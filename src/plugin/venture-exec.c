/*
 * venture-exec.c - Running an exec plugin's program
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* The chunk read from each pipe at a time. */
#define VENTURE_EXEC_READ_CHUNK (64 * 1024)

/* Secrets shorter than this are not redacted: replacing every "abc" in a
 * price feed would mangle the data to hide nothing worth hiding. */
#define VENTURE_EXEC_MIN_SECRET_LENGTH (6)

/* How much of standard error a failure message quotes. */
#define VENTURE_EXEC_STDERR_TAIL (512)

struct _VentureExecSpec
{
	gint		 ref_count;
	gchar		*name;
	gchar		*root;		/* realpath */
	gchar		*executable;	/* realpath, as at construction */
	GPtrArray	*args;		/* gchar*, after the program */
	GPtrArray	*environment;	/* "NAME=value" */
	guint		 timeout_ms;
	gsize		 max_stdout;
	gsize		 max_stderr;
};

struct _VentureExecResult
{
	gint		 ref_count;
	GPtrArray	*messages;
	gint		 exit_status;
	gchar		*stderr_text;
	gboolean	 stderr_truncated;
	gsize		 stdout_bytes;
	gchar		*error_message;
	gint64		 retry_after;
	gchar		*name;
};

G_DEFINE_BOXED_TYPE(VentureExecSpec, venture_exec_spec,
                    venture_exec_spec_ref, venture_exec_spec_unref)
G_DEFINE_BOXED_TYPE(VentureExecResult, venture_exec_result,
                    venture_exec_result_ref, venture_exec_result_unref)

/* ==========================================================================
 * Paths
 * ========================================================================== */

gchar *
venture_exec_resolve_within(
	const gchar	 *root,
	const gchar	 *path,
	GError		**error
){
	g_autofree gchar *candidate = NULL;
	g_autofree gchar *prefix = NULL;
	gchar *real_root;
	gchar *real_path;
	gchar *result;

	g_return_val_if_fail(NULL != root, NULL);
	g_return_val_if_fail(NULL != path, NULL);

	real_root = realpath(root, NULL);

	if (NULL == real_root)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "The plugin directory %s cannot be resolved: %s",
		            root, g_strerror(errno));
		return NULL;
	}

	candidate = g_path_is_absolute(path)
		? g_strdup(path) : g_build_filename(root, path, NULL);
	real_path = realpath(candidate, NULL);

	if (NULL == real_path)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "%s cannot be resolved: %s", candidate,
		            g_strerror(errno));
		free(real_root);
		return NULL;
	}

	/*
	 * Strictly inside, compared against the root plus its separator. A
	 * bare prefix test would let /srv/plug-evil/run pass for /srv/plug,
	 * and the root itself is not a program.
	 */
	prefix = g_str_has_suffix(real_root, "/")
		? g_strdup(real_root) : g_strconcat(real_root, "/", NULL);

	if (!g_str_has_prefix(real_path, prefix))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		            "%s resolves to %s, which is outside the plugin's "
		            "directory %s", path, real_path, real_root);
		free(real_root);
		free(real_path);
		return NULL;
	}

	result = g_strdup(real_path);
	free(real_root);
	free(real_path);

	return result;
}

/*
 * Resolves the executable and checks it is something that can be run. Done
 * at construction and again at every run: the file can be replaced, or a
 * directory on its path swapped for a symlink, at any time after load.
 */
static gchar *
exec_resolve_executable(
	const gchar	 *name,
	const gchar	 *root,
	const gchar	 *executable,
	GError		**error
){
	g_autofree gchar *resolved = NULL;

	resolved = venture_exec_resolve_within(root, executable, error);

	if (NULL == resolved)
		return NULL;

	if (!g_file_test(resolved, G_FILE_TEST_IS_REGULAR) ||
	    !g_file_test(resolved, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "The exec plugin %s names %s, which is not an "
		            "executable file", name, resolved);
		return NULL;
	}

	return g_steal_pointer(&resolved);
}

/* ==========================================================================
 * The spec
 * ========================================================================== */

/*
 * Adds NAME=value for a variable the server has, unless the name is
 * already there. Captured once, on the main thread: getenv() is not safe
 * against a concurrent setenv(), and a run may happen on a worker.
 */
static void
exec_spec_capture_env(
	VentureExecSpec	*self,
	const gchar	*name
){
	g_autofree gchar *prefix = NULL;
	const gchar *value;
	guint i;

	prefix = g_strconcat(name, "=", NULL);

	for (i = 0; i < self->environment->len; i++)
	{
		if (g_str_has_prefix(g_ptr_array_index(self->environment, i), prefix))
			return;
	}

	value = g_getenv(name);

	if (NULL != value)
		g_ptr_array_add(self->environment, g_strconcat(prefix, value, NULL));
}

VentureExecSpec *
venture_exec_spec_new(
	const gchar	 *name,
	const gchar	 *root,
	const gchar	 *executable,
	GError		**error
){
	VentureExecSpec *self;
	g_autofree gchar *real_root = NULL;
	g_autofree gchar *resolved = NULL;
	gchar *raw_root;

	g_return_val_if_fail(NULL != name, NULL);
	g_return_val_if_fail(NULL != root, NULL);
	g_return_val_if_fail(NULL != executable, NULL);

	resolved = exec_resolve_executable(name, root, executable, error);

	if (NULL == resolved)
		return NULL;

	/* resolve_within just proved the root resolves. */
	raw_root = realpath(root, NULL);

	if (NULL == raw_root)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "The plugin directory %s cannot be resolved", root);
		return NULL;
	}

	real_root = g_strdup(raw_root);
	free(raw_root);

	self = g_new0(VentureExecSpec, 1);
	self->ref_count = 1;
	self->name = g_strdup(name);
	self->root = g_steal_pointer(&real_root);
	self->executable = g_steal_pointer(&resolved);
	self->args = g_ptr_array_new_with_free_func(g_free);
	self->environment = g_ptr_array_new_with_free_func(g_free);
	self->timeout_ms = VENTURE_EXEC_DEFAULT_TIMEOUT_SECONDS * 1000;
	self->max_stdout = VENTURE_EXEC_DEFAULT_MAX_STDOUT;
	self->max_stderr = VENTURE_EXEC_DEFAULT_MAX_STDERR;

	/*
	 * The whole inherited environment is the server's: its database URI,
	 * its tokens, whatever the operator exported to start it. A program
	 * gets what it needs to find an interpreter and speak a language,
	 * and nothing else unless its manifest names it.
	 */
	exec_spec_capture_env(self, "PATH");
	exec_spec_capture_env(self, "LANG");
	exec_spec_capture_env(self, "HOME");

	/* With no PATH at all, `#!/usr/bin/env bash` finds nothing. */
	{
		guint i;
		gboolean have_path;

		have_path = FALSE;

		for (i = 0; i < self->environment->len; i++)
		{
			if (g_str_has_prefix(g_ptr_array_index(self->environment, i),
			                     "PATH="))
				have_path = TRUE;
		}

		if (!have_path)
			g_ptr_array_add(self->environment,
			                g_strdup("PATH=/usr/local/bin:/usr/bin:/bin"));
	}

	return self;
}

VentureExecSpec *
venture_exec_spec_ref(VentureExecSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	g_atomic_int_inc(&self->ref_count);

	return self;
}

void
venture_exec_spec_unref(VentureExecSpec *self)
{
	if (NULL == self)
		return;

	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_free(self->name);
	g_free(self->root);
	g_free(self->executable);
	g_ptr_array_unref(self->args);
	g_ptr_array_unref(self->environment);
	g_free(self);
}

void
venture_exec_spec_set_args(
	VentureExecSpec		*self,
	const gchar *const	*args
){
	gsize i;

	g_return_if_fail(NULL != self);

	g_ptr_array_set_size(self->args, 0);

	for (i = 0; (NULL != args) && (NULL != args[i]); i++)
		g_ptr_array_add(self->args, g_strdup(args[i]));
}

gboolean
venture_exec_spec_pass_env(
	VentureExecSpec	 *self,
	const gchar	 *name,
	GError		**error
){
	gsize i;

	g_return_val_if_fail(NULL != self, FALSE);
	g_return_val_if_fail(NULL != name, FALSE);

	if (!(g_ascii_isalpha(name[0]) || ('_' == name[0])))
		goto refuse;

	for (i = 1; '\0' != name[i]; i++)
	{
		if (!g_ascii_isalnum(name[i]) && ('_' != name[i]))
			goto refuse;
	}

	if (i > 128)
		goto refuse;

	exec_spec_capture_env(self, name);

	return TRUE;

refuse:
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "The exec plugin %s asks for an environment variable whose "
	            "name is not one", self->name);
	return FALSE;
}

void
venture_exec_spec_set_timeout(
	VentureExecSpec	*self,
	guint		 seconds
){
	g_return_if_fail(NULL != self);

	seconds = CLAMP(seconds, 1, VENTURE_EXEC_MAX_TIMEOUT_SECONDS);
	self->timeout_ms = seconds * 1000;
}

void
venture_exec_spec_set_timeout_ms(
	VentureExecSpec	*self,
	guint		 milliseconds
){
	g_return_if_fail(NULL != self);

	self->timeout_ms = CLAMP(milliseconds, 1,
	                         VENTURE_EXEC_MAX_TIMEOUT_SECONDS * 1000);
}

void
venture_exec_spec_set_limits(
	VentureExecSpec	*self,
	gsize		 max_stdout,
	gsize		 max_stderr
){
	g_return_if_fail(NULL != self);

	self->max_stdout = CLAMP(max_stdout, 1, VENTURE_EXEC_MAX_MAX_STDOUT);
	self->max_stderr = CLAMP(max_stderr, 1, VENTURE_EXEC_MAX_MAX_STDERR);
}

const gchar *
venture_exec_spec_get_name(VentureExecSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->name;
}

const gchar *
venture_exec_spec_get_root(VentureExecSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->root;
}

const gchar *
venture_exec_spec_get_executable(VentureExecSpec *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->executable;
}

guint
venture_exec_spec_get_timeout_ms(VentureExecSpec *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->timeout_ms;
}

gsize
venture_exec_spec_get_max_stdout(VentureExecSpec *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->max_stdout;
}

gsize
venture_exec_spec_get_max_stderr(VentureExecSpec *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->max_stderr;
}

/* ==========================================================================
 * Redaction
 * ========================================================================== */

static void
exec_collect_secrets(
	JsonNode	*node,
	GPtrArray	*out
){
	if (NULL == node)
		return;

	if (JSON_NODE_HOLDS_OBJECT(node))
	{
		JsonObjectIter iter;
		JsonNode *member;
		const gchar *name;

		json_object_iter_init(&iter, json_node_get_object(node));

		while (json_object_iter_next(&iter, &name, &member))
			exec_collect_secrets(member, out);

		return;
	}

	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array;
		guint i;

		array = json_node_get_array(node);

		for (i = 0; i < json_array_get_length(array); i++)
			exec_collect_secrets(json_array_get_element(array, i), out);

		return;
	}

	if (JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_STRING == json_node_get_value_type(node)) &&
	    (strlen(json_node_get_string(node)) >= VENTURE_EXEC_MIN_SECRET_LENGTH))
		g_ptr_array_add(out, g_strdup(json_node_get_string(node)));
}

/* Longest first, so a secret containing another is replaced whole. */
static gint
exec_compare_length(
	gconstpointer	a,
	gconstpointer	b
){
	gsize left;
	gsize right;

	left = strlen(*(const gchar *const *)a);
	right = strlen(*(const gchar *const *)b);

	return (left < right) ? 1 : ((left > right) ? -1 : 0);
}

/*
 * Returns a copy of @text with every secret replaced, or NULL when none
 * occurs -- the common case, which then allocates nothing.
 */
static gchar *
exec_redact_text(
	const gchar	*text,
	GPtrArray	*secrets
){
	g_autoptr(GString) buffer = NULL;
	guint i;

	if ((NULL == text) || (0 == secrets->len))
		return NULL;

	for (i = 0; i < secrets->len; i++)
	{
		const gchar *secret;

		secret = g_ptr_array_index(secrets, i);

		if (NULL == strstr((NULL != buffer) ? buffer->str : text, secret))
			continue;

		if (NULL == buffer)
			buffer = g_string_new(text);

		g_string_replace(buffer, secret, VENTURE_EXEC_REDACTED, 0);
	}

	return (NULL != buffer) ? g_string_free(g_steal_pointer(&buffer), FALSE)
	                        : NULL;
}

/*
 * Redacts every string in a message, members and nested values alike. A
 * program that echoes its request back -- in a record, a log line, an error
 * -- would otherwise hand its secret to whatever stores or logs the batch.
 */
static void
exec_redact_node(
	JsonNode	*node,
	GPtrArray	*secrets
){
	if (NULL == node)
		return;

	if (JSON_NODE_HOLDS_OBJECT(node))
	{
		JsonObjectIter iter;
		JsonNode *member;
		const gchar *name;

		json_object_iter_init(&iter, json_node_get_object(node));

		while (json_object_iter_next(&iter, &name, &member))
			exec_redact_node(member, secrets);

		return;
	}

	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array;
		guint i;

		array = json_node_get_array(node);

		for (i = 0; i < json_array_get_length(array); i++)
			exec_redact_node(json_array_get_element(array, i), secrets);

		return;
	}

	if (JSON_NODE_HOLDS_VALUE(node) &&
	    (G_TYPE_STRING == json_node_get_value_type(node)))
	{
		g_autofree gchar *redacted = NULL;

		redacted = exec_redact_text(json_node_get_string(node), secrets);

		if (NULL != redacted)
			json_node_set_string(node, redacted);
	}
}

/* ==========================================================================
 * A run
 * ========================================================================== */

typedef struct
{
	VentureExecSpec		*spec;
	GMainContext		*context;
	GCancellable		*io_cancellable;
	GSubprocess		*process;
	GSocket			*stdin_socket;
	GSocketConnection	*stdin_connection;
	GBytes			*request;
	VentureJsonlReader	*reader;
	GPtrArray		*messages;
	GByteArray		*stderr_buffer;
	gboolean		 stderr_truncated;
	gsize			 stdout_bytes;
	guint			 pending;
	gboolean		 stdout_done;
	gboolean		 stderr_done;
	gboolean		 exited;
	gboolean		 killed;
	pid_t			 pid;
	GError			*failure;
} ExecRun;

/*
 * Ends everything the program started. The program runs as the leader of
 * its own process group, so this reaches the `sleep` a shell script left
 * behind as well as the script -- a grandchild that kept the output pipe
 * open would otherwise keep the run waiting for an end-of-file that never
 * comes.
 */
static void
exec_run_kill(ExecRun *run)
{
	if (run->killed || (run->pid <= 0))
		return;

	run->killed = TRUE;

	/*
	 * Once the leader is reaped its pid can in principle be reused, so the
	 * group is signalled only while something may still hold a pipe:
	 * the leader is alive, or an output has not reached its end.
	 */
	if (!run->exited || !run->stdout_done || !run->stderr_done)
		(void)kill(-run->pid, SIGKILL);

	/* setpgid may not have happened yet if the child is brand new; this
	 * reaches the leader whatever group it is in. */
	if (!run->exited)
		g_subprocess_force_exit(run->process);
}

/*
 * Records the first failure and tears the run down. Later failures are
 * consequences of the first -- a read cancelled because the run timed out
 * -- and would only obscure it.
 */
static void
exec_run_fail(
	ExecRun	*run,
	GError	*error
){
	if (NULL == run->failure)
		run->failure = error;
	else
		g_error_free(error);

	exec_run_kill(run);
	g_cancellable_cancel(run->io_cancellable);
}

static void exec_run_read_stdout(ExecRun *run);
static void exec_run_read_stderr(ExecRun *run);

static void
exec_on_stdout(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	ExecRun *run;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	const guint8 *data;
	gsize size;

	run = user_data;
	run->pending--;

	bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result,
	                                         &error);

	if (NULL == bytes)
	{
		run->stdout_done = TRUE;

		if (NULL == run->failure)
			exec_run_fail(run, g_error_new(VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			              "Reading the output of %s failed: %s",
			              run->spec->name, error->message));
		return;
	}

	if (NULL != run->failure)
	{
		run->stdout_done = TRUE;
		return;
	}

	data = g_bytes_get_data(bytes, &size);

	if (0 == size)
	{
		run->stdout_done = TRUE;

		if (!venture_jsonl_reader_finish(run->reader, run->messages, &error))
			exec_run_fail(run, g_error_new(VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			              "%s broke the protocol: %s", run->spec->name,
			              error->message));
		return;
	}

	run->stdout_bytes += size;

	/* Fails rather than truncates: a batch cut at the cap looks exactly
	 * like a complete one to whatever reads it. */
	if (run->stdout_bytes > run->spec->max_stdout)
	{
		run->stdout_done = TRUE;
		exec_run_fail(run, g_error_new(VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		              "%s wrote more than %" G_GSIZE_FORMAT " bytes of output",
		              run->spec->name, run->spec->max_stdout));
		return;
	}

	if (!venture_jsonl_reader_feed(run->reader, data, size, run->messages,
	                               &error))
	{
		run->stdout_done = TRUE;
		exec_run_fail(run, g_error_new(VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		              "%s broke the protocol: %s", run->spec->name,
		              error->message));
		return;
	}

	exec_run_read_stdout(run);
}

static void
exec_run_read_stdout(ExecRun *run)
{
	run->pending++;
	g_input_stream_read_bytes_async(
		g_subprocess_get_stdout_pipe(run->process), VENTURE_EXEC_READ_CHUNK,
		G_PRIORITY_DEFAULT, run->io_cancellable, exec_on_stdout, run);
}

static void
exec_on_stderr(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	ExecRun *run;
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	const guint8 *data;
	gsize size;

	run = user_data;
	run->pending--;

	bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result,
	                                         &error);

	/* A failed stderr read loses diagnostics, not data: it ends the
	 * stream without failing the run. */
	if ((NULL == bytes) || (NULL != run->failure))
	{
		run->stderr_done = TRUE;
		return;
	}

	data = g_bytes_get_data(bytes, &size);

	if (0 == size)
	{
		run->stderr_done = TRUE;
		return;
	}

	/* Kept up to the cap and drained past it, so a program that logs a
	 * great deal is not blocked on a full pipe and then killed for it. */
	if (run->stderr_buffer->len < run->spec->max_stderr)
	{
		gsize room;

		room = run->spec->max_stderr - run->stderr_buffer->len;

		if (size > room)
			run->stderr_truncated = TRUE;

		g_byte_array_append(run->stderr_buffer, data, (guint)MIN(size, room));
	}
	else
	{
		run->stderr_truncated = TRUE;
	}

	exec_run_read_stderr(run);
}

static void
exec_run_read_stderr(ExecRun *run)
{
	run->pending++;
	g_input_stream_read_bytes_async(
		g_subprocess_get_stderr_pipe(run->process), VENTURE_EXEC_READ_CHUNK,
		G_PRIORITY_DEFAULT, run->io_cancellable, exec_on_stderr, run);
}

static void
exec_on_exit(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	ExecRun *run;

	run = user_data;
	run->pending--;

	/* Cancelled only on the failure path; GSubprocess still reaps the
	 * child on its own, so nothing is left as a zombie. */
	if (g_subprocess_wait_finish(G_SUBPROCESS(source), result, NULL))
		run->exited = TRUE;
}

static void
exec_on_stdin_written(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	ExecRun *run;

	run = user_data;
	run->pending--;

	/*
	 * A program that never reads its request is allowed to: the write then
	 * fails with a broken pipe, which is the program's choice and not a
	 * failure of the run. The socket is why that is an error code here
	 * and not a SIGPIPE that kills the server.
	 */
	(void)g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result,
	                                       NULL, NULL);
	(void)g_socket_shutdown(run->stdin_socket, FALSE, TRUE, NULL);
}

static gboolean
exec_on_timeout(gpointer user_data)
{
	ExecRun *run;

	run = user_data;

	exec_run_fail(run, g_error_new(VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
	              "%s did not finish within %u ms and was stopped",
	              run->spec->name, run->spec->timeout_ms));

	return G_SOURCE_REMOVE;
}

static gboolean
exec_on_cancelled(
	GCancellable	*cancellable,
	gpointer	 user_data
){
	ExecRun *run;

	run = user_data;

	exec_run_fail(run, g_error_new(G_IO_ERROR, G_IO_ERROR_CANCELLED,
	              "The run of %s was cancelled", run->spec->name));

	return G_SOURCE_REMOVE;
}

static gboolean
exec_run_done(ExecRun *run)
{
	if (0 != run->pending)
		return FALSE;

	if (NULL != run->failure)
		return TRUE;

	return run->exited && run->stdout_done && run->stderr_done;
}

static void
exec_child_setup(gpointer user_data)
{
	(void)user_data;

	/* Its own process group, so the whole tree can be stopped at once. */
	if (0 != setpgid(0, 0))
		_exit(126);
}

/*
 * Serialises the request line: the caller's members, the protocol version,
 * and the secrets. Built fresh rather than added to the caller's object,
 * which may be shared and must not come back holding secrets.
 */
static GBytes *
exec_build_request(
	JsonObject	*request,
	JsonObject	*secrets
){
	g_autoptr(JsonGenerator) generator = NULL;
	g_autoptr(JsonNode) root = NULL;
	g_autofree gchar *text = NULL;
	JsonObject *object;
	gsize length;

	object = json_object_new();
	json_object_set_int_member(object, "protocol",
	                           VENTURE_JSONL_PROTOCOL_VERSION);

	if (NULL != request)
	{
		JsonObjectIter iter;
		JsonNode *member;
		const gchar *name;

		json_object_iter_init(&iter, request);

		while (json_object_iter_next(&iter, &name, &member))
		{
			if ((0 == g_strcmp0(name, "protocol")) ||
			    (0 == g_strcmp0(name, "secrets")))
				continue;

			json_object_set_member(object, name, json_node_copy(member));
		}
	}

	if (NULL != secrets)
	{
		JsonNode *node;

		node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(node, secrets);
		json_object_set_member(object, "secrets", json_node_copy(node));
		json_node_unref(node);
	}
	else
	{
		json_object_set_object_member(object, "secrets", json_object_new());
	}

	root = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(root, object);

	generator = json_generator_new();
	json_generator_set_root(generator, root);
	text = json_generator_to_data(generator, &length);

	/* One line: the generator escapes newlines inside strings, so the
	 * only newline is the terminator added here. */
	return g_bytes_new_take(g_strconcat(text, "\n", NULL), length + 1);
}

/*
 * Starts the program with its standard input on one end of a socket pair.
 * A pipe would do the same job until the program exits without reading:
 * then the write raises SIGPIPE, which kills the server. A socket write
 * passes MSG_NOSIGNAL and gets EPIPE back instead.
 */
static gboolean
exec_run_spawn(
	ExecRun		 *run,
	const gchar	 *executable,
	GError		**error
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GPtrArray) argv = NULL;
	g_autoptr(GPtrArray) env_vector = NULL;
	const gchar *identifier;
	gint fds[2];
	guint i;

	if (0 != socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "Cannot create the input of %s: %s", run->spec->name,
		            g_strerror(errno));
		return FALSE;
	}

	run->stdin_socket = g_socket_new_from_fd(fds[0], error);

	if (NULL == run->stdin_socket)
	{
		close(fds[0]);
		close(fds[1]);
		return FALSE;
	}

	argv = g_ptr_array_new();
	g_ptr_array_add(argv, (gpointer)executable);

	for (i = 0; i < run->spec->args->len; i++)
		g_ptr_array_add(argv, g_ptr_array_index(run->spec->args, i));

	g_ptr_array_add(argv, NULL);

	env_vector = g_ptr_array_new();

	for (i = 0; i < run->spec->environment->len; i++)
		g_ptr_array_add(env_vector, g_ptr_array_index(run->spec->environment, i));

	g_ptr_array_add(env_vector, NULL);

	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                                     G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_subprocess_launcher_set_environ(launcher, (gchar **)env_vector->pdata);
	g_subprocess_launcher_set_cwd(launcher, run->spec->root);
	g_subprocess_launcher_set_child_setup(launcher, exec_child_setup,
	                                      NULL, NULL);
	g_subprocess_launcher_take_stdin_fd(launcher, fds[1]);

	run->process = g_subprocess_launcher_spawnv(
		launcher, (const gchar *const *)argv->pdata, error);

	if (NULL == run->process)
		return FALSE;

	identifier = g_subprocess_get_identifier(run->process);
	run->pid = (NULL != identifier) ? (pid_t)g_ascii_strtoll(identifier,
	                                                         NULL, 10) : 0;

	/* Both sides set the group, the classic way to close the window
	 * between fork and the child's own setpgid. The child may already
	 * have exec'd, which makes this fail harmlessly. */
	if (run->pid > 0)
		(void)setpgid(run->pid, run->pid);

	return TRUE;
}

VentureExecResult *
venture_exec_run(
	VentureExecSpec	 *spec,
	JsonObject	 *request,
	JsonObject	 *secrets,
	GCancellable	 *cancellable,
	GError		**error
){
	g_autoptr(GPtrArray) secret_values = NULL;
	g_autofree gchar *executable = NULL;
	g_autoptr(GError) spawn_error = NULL;
	GSource *timeout_source = NULL;
	GSource *cancel_source = NULL;
	VentureExecResult *result;
	ExecRun run;
	guint i;

	g_return_val_if_fail(NULL != spec, NULL);

	/* Checked again now, not trusted from load: see the header. */
	executable = exec_resolve_executable(spec->name, spec->root,
	                                     spec->executable, error);

	if (NULL == executable)
		return NULL;

	if ((NULL != cancellable) && g_cancellable_is_cancelled(cancellable))
	{
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
		            "The run of %s was cancelled", spec->name);
		return NULL;
	}

	secret_values = g_ptr_array_new_with_free_func(g_free);

	if (NULL != secrets)
	{
		JsonNode *node;

		node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(node, secrets);
		exec_collect_secrets(node, secret_values);
		json_node_unref(node);
		g_ptr_array_sort(secret_values, exec_compare_length);
	}

	/* Every member filled by hand: the struct is a bare local. */
	run.spec = spec;
	run.context = g_main_context_new();
	run.io_cancellable = g_cancellable_new();
	run.process = NULL;
	run.stdin_socket = NULL;
	run.stdin_connection = NULL;
	run.request = exec_build_request(request, secrets);
	run.reader = venture_jsonl_reader_new(spec->max_stdout);
	run.messages = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_jsonl_message_unref);
	run.stderr_buffer = g_byte_array_new();
	run.stderr_truncated = FALSE;
	run.stdout_bytes = 0;
	run.pending = 0;
	run.stdout_done = FALSE;
	run.stderr_done = FALSE;
	run.exited = FALSE;
	run.killed = FALSE;
	run.pid = 0;
	run.failure = NULL;

	/*
	 * A private context, pushed for the duration: every callback below is
	 * dispatched here, and iterating it runs nothing else. Iterating the
	 * default context instead would run the web server's sources and the
	 * automation engine's nested loop inside this call.
	 */
	g_main_context_push_thread_default(run.context);

	if (!exec_run_spawn(&run, executable, &spawn_error))
	{
		run.failure = g_error_new(VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		                          "Cannot start %s: %s", spec->name,
		                          spawn_error->message);
	}
	else
	{
		gsize length;
		const guint8 *data;

		run.stdin_connection = g_socket_connection_factory_create_connection(
			run.stdin_socket);
		data = g_bytes_get_data(run.request, &length);

		run.pending++;
		g_output_stream_write_all_async(
			g_io_stream_get_output_stream(G_IO_STREAM(run.stdin_connection)),
			data, length, G_PRIORITY_DEFAULT, run.io_cancellable,
			exec_on_stdin_written, &run);

		exec_run_read_stdout(&run);
		exec_run_read_stderr(&run);

		run.pending++;
		g_subprocess_wait_async(run.process, run.io_cancellable,
		                        exec_on_exit, &run);

		timeout_source = g_timeout_source_new(spec->timeout_ms);
		g_source_set_callback(timeout_source, exec_on_timeout, &run, NULL);
		g_source_attach(timeout_source, run.context);

		if (NULL != cancellable)
		{
			cancel_source = g_cancellable_source_new(cancellable);
			g_source_set_callback(cancel_source,
			                      G_SOURCE_FUNC(exec_on_cancelled),
			                      &run, NULL);
			g_source_attach(cancel_source, run.context);
		}

		while (!exec_run_done(&run))
			g_main_context_iteration(run.context, TRUE);
	}

	if (NULL != timeout_source)
	{
		g_source_destroy(timeout_source);
		g_source_unref(timeout_source);
	}

	if (NULL != cancel_source)
	{
		g_source_destroy(cancel_source);
		g_source_unref(cancel_source);
	}

	if (NULL != run.stdin_socket)
		(void)g_socket_close(run.stdin_socket, NULL);

	g_main_context_pop_thread_default(run.context);

	result = NULL;

	if (NULL != run.failure)
	{
		g_autofree gchar *redacted = NULL;

		/* An error message can carry what the program printed; it goes
		 * through the same redaction as everything else. */
		redacted = exec_redact_text(run.failure->message, secret_values);

		if (NULL != redacted)
		{
			g_free(run.failure->message);
			run.failure->message = g_steal_pointer(&redacted);
		}

		g_propagate_error(error, run.failure);
		run.failure = NULL;
	}
	else
	{
		g_autofree gchar *stderr_text = NULL;
		g_autofree gchar *redacted = NULL;

		result = g_new0(VentureExecResult, 1);
		result->ref_count = 1;
		result->name = g_strdup(spec->name);
		result->messages = g_ptr_array_ref(run.messages);
		result->stdout_bytes = run.stdout_bytes;
		result->stderr_truncated = run.stderr_truncated;
		result->retry_after = -1;

		if (g_subprocess_get_if_exited(run.process))
			result->exit_status = g_subprocess_get_exit_status(run.process);
		else if (g_subprocess_get_if_signaled(run.process))
			result->exit_status = -g_subprocess_get_term_sig(run.process);
		else
			result->exit_status = -1;

		/* An empty GByteArray has no data pointer at all. */
		stderr_text = (0 == run.stderr_buffer->len)
			? g_strdup("")
			: g_utf8_make_valid((const gchar *)run.stderr_buffer->data,
			                    (gssize)run.stderr_buffer->len);
		redacted = exec_redact_text(stderr_text, secret_values);
		result->stderr_text = (NULL != redacted) ? g_steal_pointer(&redacted)
		                                         : g_steal_pointer(&stderr_text);

		for (i = 0; i < run.messages->len; i++)
		{
			VentureJsonlMessage *message;
			JsonNode *node;

			message = g_ptr_array_index(run.messages, i);

			if (secret_values->len > 0)
			{
				node = json_node_new(JSON_NODE_OBJECT);
				json_node_set_object(node,
					venture_jsonl_message_get_object(message));
				exec_redact_node(node, secret_values);
				json_node_unref(node);
			}

			if ((NULL == result->error_message) &&
			    (VENTURE_JSONL_MESSAGE_ERROR ==
			     venture_jsonl_message_get_kind(message)))
			{
				result->error_message = g_strdup(
					venture_jsonl_message_get_string(message, "message"));
				result->retry_after = venture_jsonl_message_get_int(
					message, "retry_after", -1);
			}
		}
	}

	g_clear_object(&run.stdin_connection);
	g_clear_object(&run.stdin_socket);
	g_clear_object(&run.process);
	g_clear_object(&run.io_cancellable);
	g_bytes_unref(run.request);
	venture_jsonl_reader_free(run.reader);
	g_ptr_array_unref(run.messages);
	g_byte_array_unref(run.stderr_buffer);
	g_main_context_unref(run.context);

	return result;
}

/* ==========================================================================
 * The result
 * ========================================================================== */

VentureExecResult *
venture_exec_result_ref(VentureExecResult *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	g_atomic_int_inc(&self->ref_count);

	return self;
}

void
venture_exec_result_unref(VentureExecResult *self)
{
	if (NULL == self)
		return;

	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_ptr_array_unref(self->messages);
	g_free(self->stderr_text);
	g_free(self->error_message);
	g_free(self->name);
	g_free(self);
}

GPtrArray *
venture_exec_result_get_messages(VentureExecResult *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->messages;
}

gint
venture_exec_result_get_exit_status(VentureExecResult *self)
{
	g_return_val_if_fail(NULL != self, -1);

	return self->exit_status;
}

const gchar *
venture_exec_result_get_stderr(VentureExecResult *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->stderr_text;
}

gboolean
venture_exec_result_get_stderr_truncated(VentureExecResult *self)
{
	g_return_val_if_fail(NULL != self, FALSE);

	return self->stderr_truncated;
}

gsize
venture_exec_result_get_stdout_bytes(VentureExecResult *self)
{
	g_return_val_if_fail(NULL != self, 0);

	return self->stdout_bytes;
}

const gchar *
venture_exec_result_get_error_message(VentureExecResult *self)
{
	g_return_val_if_fail(NULL != self, NULL);

	return self->error_message;
}

gint64
venture_exec_result_get_retry_after(VentureExecResult *self)
{
	g_return_val_if_fail(NULL != self, -1);

	return self->retry_after;
}

/*
 * The last few hundred bytes of standard error, where a failing program
 * usually says why.
 */
static const gchar *
exec_result_stderr_tail(VentureExecResult *self)
{
	const gchar *text;
	gsize length;

	text = self->stderr_text;
	length = strlen(text);

	if (length <= VENTURE_EXEC_STDERR_TAIL)
		return text;

	text += length - VENTURE_EXEC_STDERR_TAIL;

	/* Not in the middle of a character. */
	while (('\0' != *text) && (0x80 == (((guchar)*text) & 0xC0)))
		text++;

	return text;
}

gboolean
venture_exec_result_check(
	VentureExecResult	 *self,
	GError			**error
){
	g_return_val_if_fail(NULL != self, FALSE);

	if (NULL != self->error_message)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
		            "%s reported an error: %s", self->name,
		            self->error_message);
		return FALSE;
	}

	if (0 != self->exit_status)
	{
		g_autofree gchar *tail = NULL;

		tail = g_strstrip(g_strdup(exec_result_stderr_tail(self)));

		if (self->exit_status > 0)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s exited with status %d%s%s", self->name,
			            self->exit_status, ('\0' != tail[0]) ? ": " : "",
			            tail);
		else
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PLUGIN,
			            "%s was ended by signal %d%s%s", self->name,
			            -self->exit_status, ('\0' != tail[0]) ? ": " : "",
			            tail);
		return FALSE;
	}

	return TRUE;
}
