/*
 * venture-work-tools.c - Confined file tools
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

#include <string.h>

/* Enough for source files; large enough that a real one is never truncated,
 * small enough that a stray binary cannot exhaust memory. */
#define VENTURE_WORK_TOOLS_MAX_BYTES (2 * 1024 * 1024)

/* What a grep or a glob will report before it stops. A model that asked a
 * question broad enough to match ten thousand files needs a narrower
 * question, not ten thousand answers. */
#define VENTURE_WORK_TOOLS_MAX_HITS (200)

typedef struct
{
	gchar *root;
} VentureWorkToolsCtx;

static void
venture_work_tools_ctx_free(gpointer data)
{
	VentureWorkToolsCtx *ctx = data;

	g_free(ctx->root);
	g_free(ctx);
}

/*
 * Canonicalises a path that may not exist yet.
 *
 * realpath() fails on a missing final component, which is exactly the case
 * for a file being written for the first time. Resolving the parent and
 * re-attaching the basename gives a canonical answer for a path that does
 * not exist, without inventing one for a parent that does not either.
 */
static gchar *
venture_work_tools_canonical(
	const gchar	*path,
	gboolean	 must_exist
){
	g_autofree gchar *parent = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *real_parent = NULL;

	if (g_file_test(path, G_FILE_TEST_EXISTS))
		return realpath(path, NULL);

	if (must_exist)
		return NULL;

	parent = g_path_get_dirname(path);
	base = g_path_get_basename(path);
	real_parent = realpath(parent, NULL);

	if (NULL == real_parent)
		return NULL;

	return g_build_filename(real_parent, base, NULL);
}

gchar *
venture_work_tools_resolve(
	const gchar	 *root,
	const gchar	 *path,
	gboolean	  must_exist,
	GError		**error
){
	g_autofree gchar *joined = NULL;
	g_autofree gchar *canonical = NULL;
	g_autofree gchar *real_root = NULL;
	g_autofree gchar *bounded = NULL;

	if (venture_string_is_empty(root))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "No workspace to resolve against");
		return NULL;
	}

	if (venture_string_is_empty(path))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "No path given");
		return NULL;
	}

	real_root = realpath(root, NULL);

	if (NULL == real_root)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "The workspace \"%s\" is not there", root);
		return NULL;
	}

	/* An absolute path is taken at face value and then checked, rather
	 * than rejected outright: the agent is told the workspace's real
	 * path, so naming a file inside it absolutely is reasonable. */
	joined = g_path_is_absolute(path)
		? g_strdup(path)
		: g_build_filename(real_root, path, NULL);

	canonical = venture_work_tools_canonical(joined, must_exist);

	if (NULL == canonical)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "\"%s\" is not there", path);
		return NULL;
	}

	/*
	 * The separator is what makes this a containment test rather than a
	 * string prefix test. Without it a root of "/w/run-1" accepts
	 * "/w/run-1-evil", which an agent can create and then read anything
	 * from -- the classic version of this bug.
	 */
	bounded = g_strconcat(real_root, G_DIR_SEPARATOR_S, NULL);

	if ((0 != g_strcmp0(canonical, real_root)) &&
	    !g_str_has_prefix(canonical, bounded))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		            "\"%s\" is outside the workspace", path);
		return NULL;
	}

	return g_steal_pointer(&canonical);
}

static gchar *
venture_work_tools_error(
	const gchar *format,
	...
) G_GNUC_PRINTF(1, 2);

static gchar *
venture_work_tools_error(
	const gchar *format,
	...
){
	g_autofree gchar *detail = NULL;
	va_list args;

	va_start(args, format);
	detail = g_strdup_vprintf(format, args);
	va_end(args);

	/* Returned as the tool's result rather than raised as an error: a
	 * model that is told why its call was refused usually adjusts, and a
	 * model that is told nothing retries the same call. */
	return g_strdup_printf("Error: %s", detail);
}

static gchar *
venture_work_tool_read(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureWorkToolsCtx *ctx = user_data;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *resolved = NULL;
	g_autofree gchar *contents = NULL;
	const gchar *path;
	gsize length = 0;

	(void)cancellable;
	(void)error;

	path = ai_tool_use_get_input_string(tool_use, "path");
	resolved = venture_work_tools_resolve(ctx->root, path, TRUE, &local_error);

	if (NULL == resolved)
		return venture_work_tools_error("%s", local_error->message);

	if (!g_file_get_contents(resolved, &contents, &length, &local_error))
		return venture_work_tools_error("%s", local_error->message);

	if (length > VENTURE_WORK_TOOLS_MAX_BYTES)
	{
		return venture_work_tools_error(
			"\"%s\" is %" G_GSIZE_FORMAT " bytes, which is too big to read "
			"in one go", path, length);
	}

	if (!g_utf8_validate(contents, (gssize)length, NULL))
		return venture_work_tools_error("\"%s\" is not text", path);

	return g_steal_pointer(&contents);
}

static gchar *
venture_work_tool_write(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureWorkToolsCtx *ctx = user_data;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *resolved = NULL;
	g_autofree gchar *parent = NULL;
	const gchar *path;
	const gchar *contents;

	(void)cancellable;
	(void)error;

	path = ai_tool_use_get_input_string(tool_use, "path");
	contents = ai_tool_use_get_input_string(tool_use, "contents");

	if (NULL == contents)
		contents = "";

	/* must_exist is FALSE: writing a new file is the common case, and
	 * the parent is canonicalised in its place. */
	resolved = venture_work_tools_resolve(ctx->root, path, FALSE,
	                                      &local_error);

	if (NULL == resolved)
		return venture_work_tools_error("%s", local_error->message);

	parent = g_path_get_dirname(resolved);

	if (0 != g_mkdir_with_parents(parent, 0755))
		return venture_work_tools_error("Cannot create \"%s\"", parent);

	if (!g_file_set_contents(resolved, contents, -1, &local_error))
		return venture_work_tools_error("%s", local_error->message);

	return g_strdup_printf("Wrote %s", path);
}

static gchar *
venture_work_tool_edit(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureWorkToolsCtx *ctx = user_data;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *resolved = NULL;
	g_autofree gchar *contents = NULL;
	g_autofree gchar *replaced = NULL;
	const gchar *path;
	const gchar *find;
	const gchar *replace;
	const gchar *first;

	(void)cancellable;
	(void)error;

	path = ai_tool_use_get_input_string(tool_use, "path");
	find = ai_tool_use_get_input_string(tool_use, "find");
	replace = ai_tool_use_get_input_string(tool_use, "replace");

	if (venture_string_is_empty(find))
		return venture_work_tools_error("\"find\" is required");

	if (NULL == replace)
		replace = "";

	resolved = venture_work_tools_resolve(ctx->root, path, TRUE, &local_error);

	if (NULL == resolved)
		return venture_work_tools_error("%s", local_error->message);

	if (!g_file_get_contents(resolved, &contents, NULL, &local_error))
		return venture_work_tools_error("%s", local_error->message);

	first = strstr(contents, find);

	if (NULL == first)
		return venture_work_tools_error("\"%s\" does not contain that text",
		                                path);

	/*
	 * Refused when the text appears more than once. An edit that silently
	 * changed the first of several identical lines is how a model
	 * believes it has fixed something it has not, and the mistake only
	 * surfaces later as a puzzling diff.
	 */
	if (NULL != strstr(first + strlen(find), find))
	{
		return venture_work_tools_error(
			"That text appears more than once in \"%s\"; include enough "
			"context to name one place", path);
	}

	{
		g_autoptr(GString) out = NULL;

		out = g_string_new(contents);
		g_string_replace(out, find, replace, 1);
		replaced = g_string_free(g_steal_pointer(&out), FALSE);
	}

	if (!g_file_set_contents(resolved, replaced, -1, &local_error))
		return venture_work_tools_error("%s", local_error->message);

	return g_strdup_printf("Edited %s", path);
}

/*
 * Walks the workspace, calling @visit for each regular file.
 *
 * Directories that only ever contain machinery are skipped: .git is the
 * repository's own storage, and walking it wastes the hit budget on objects
 * no agent has any use for.
 */
static void
venture_work_tools_walk(
	const gchar	*directory,
	const gchar	*root,
	GPtrArray	*results,
	gboolean	 (*visit)(const gchar *full, const gchar *relative,
	                          GPtrArray *results, gpointer data),
	gpointer	 data
){
	g_autoptr(GDir) dir = NULL;
	const gchar *name;

	if (results->len >= VENTURE_WORK_TOOLS_MAX_HITS)
		return;

	dir = g_dir_open(directory, 0, NULL);

	if (NULL == dir)
		return;

	while (NULL != (name = g_dir_read_name(dir)))
	{
		g_autofree gchar *full = NULL;

		if ((0 == g_strcmp0(name, ".git")) ||
		    (0 == g_strcmp0(name, "node_modules")))
			continue;

		full = g_build_filename(directory, name, NULL);

		if (g_file_test(full, G_FILE_TEST_IS_SYMLINK))
			continue;

		if (g_file_test(full, G_FILE_TEST_IS_DIR))
		{
			venture_work_tools_walk(full, root, results, visit, data);
			continue;
		}

		if (results->len >= VENTURE_WORK_TOOLS_MAX_HITS)
			return;

		{
			const gchar *relative = full + strlen(root);

			while ('/' == *relative)
				relative++;

			if (!visit(full, relative, results, data))
				return;
		}
	}
}

static gboolean
venture_work_tools_visit_glob(
	const gchar	*full,
	const gchar	*relative,
	GPtrArray	*results,
	gpointer	 data
){
	GPatternSpec *pattern = data;
	g_autofree gchar *base = NULL;

	(void)full;

	base = g_path_get_basename(relative);

	if (g_pattern_spec_match_string(pattern, relative) ||
	    g_pattern_spec_match_string(pattern, base))
		g_ptr_array_add(results, g_strdup(relative));

	return TRUE;
}

static gchar *
venture_work_tool_glob(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureWorkToolsCtx *ctx = user_data;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(GPatternSpec) pattern = NULL;
	const gchar *glob;

	(void)cancellable;
	(void)error;

	glob = ai_tool_use_get_input_string(tool_use, "pattern");

	if (venture_string_is_empty(glob))
		return venture_work_tools_error("\"pattern\" is required");

	pattern = g_pattern_spec_new(glob);
	results = g_ptr_array_new_with_free_func(g_free);

	venture_work_tools_walk(ctx->root, ctx->root, results,
	                        venture_work_tools_visit_glob, pattern);

	if (0 == results->len)
		return g_strdup("No files match.");

	g_ptr_array_add(results, NULL);

	return g_strjoinv("\n", (GStrv)results->pdata);
}

typedef struct
{
	const gchar *needle;
} GrepData;

static gboolean
venture_work_tools_visit_grep(
	const gchar	*full,
	const gchar	*relative,
	GPtrArray	*results,
	gpointer	 data
){
	GrepData *grep = data;
	g_autofree gchar *contents = NULL;
	g_auto(GStrv) lines = NULL;
	gsize length = 0;
	gsize i;

	if (!g_file_get_contents(full, &contents, &length, NULL))
		return TRUE;

	if (length > VENTURE_WORK_TOOLS_MAX_BYTES)
		return TRUE;

	if (!g_utf8_validate(contents, (gssize)length, NULL))
		return TRUE;

	lines = g_strsplit(contents, "\n", -1);

	for (i = 0; NULL != lines[i]; i++)
	{
		if (NULL == strstr(lines[i], grep->needle))
			continue;

		g_ptr_array_add(results,
			g_strdup_printf("%s:%" G_GSIZE_FORMAT ": %s", relative, i + 1,
			                lines[i]));

		if (results->len >= VENTURE_WORK_TOOLS_MAX_HITS)
			return FALSE;
	}

	return TRUE;
}

static gchar *
venture_work_tool_grep(
	AiToolUse	 *tool_use,
	GCancellable	 *cancellable,
	GError		**error,
	gpointer	  user_data
){
	VentureWorkToolsCtx *ctx = user_data;
	g_autoptr(GPtrArray) results = NULL;
	GrepData grep;
	const gchar *needle;

	(void)cancellable;
	(void)error;

	needle = ai_tool_use_get_input_string(tool_use, "text");

	if (venture_string_is_empty(needle))
		return venture_work_tools_error("\"text\" is required");

	grep.needle = needle;
	results = g_ptr_array_new_with_free_func(g_free);

	venture_work_tools_walk(ctx->root, ctx->root, results,
	                        venture_work_tools_visit_grep, &grep);

	if (0 == results->len)
		return g_strdup("No matches.");

	g_ptr_array_add(results, NULL);

	return g_strjoinv("\n", (GStrv)results->pdata);
}

void
venture_work_tools_register(
	AiToolExecutor	*executor,
	const gchar	*root
){
	VentureWorkToolsCtx *ctx;
	AiTool *tool;

	g_return_if_fail(NULL != executor);
	g_return_if_fail(!venture_string_is_empty(root));

	ctx = g_new0(VentureWorkToolsCtx, 1);
	ctx->root = g_strdup(root);

	tool = ai_tool_new("read", "Read a text file from the workspace.");
	ai_tool_add_parameter(tool, "path", "string",
	                      "Path relative to the workspace root.", TRUE);
	ai_tool_executor_register_callback(executor, tool, venture_work_tool_read,
	                                   ctx, NULL);

	tool = ai_tool_new("write",
	                   "Write a file in the workspace, replacing it entirely.");
	ai_tool_add_parameter(tool, "path", "string",
	                      "Path relative to the workspace root.", TRUE);
	ai_tool_add_parameter(tool, "contents", "string", "The new contents.",
	                      TRUE);
	ai_tool_executor_register_callback(executor, tool, venture_work_tool_write,
	                                   ctx, NULL);

	tool = ai_tool_new("edit",
	                   "Replace one unique passage of text in a file. The text "
	                   "must appear exactly once.");
	ai_tool_add_parameter(tool, "path", "string",
	                      "Path relative to the workspace root.", TRUE);
	ai_tool_add_parameter(tool, "find", "string",
	                      "The exact text to replace.", TRUE);
	ai_tool_add_parameter(tool, "replace", "string", "What to put there.",
	                      TRUE);
	ai_tool_executor_register_callback(executor, tool, venture_work_tool_edit,
	                                   ctx, NULL);

	tool = ai_tool_new("glob", "List workspace files matching a pattern.");
	ai_tool_add_parameter(tool, "pattern", "string",
	                      "A glob such as *.c or src/*.h.", TRUE);
	ai_tool_executor_register_callback(executor, tool, venture_work_tool_glob,
	                                   ctx, NULL);

	tool = ai_tool_new("grep", "Search workspace files for literal text.");
	ai_tool_add_parameter(tool, "text", "string", "The text to look for.",
	                      TRUE);
	/* The last registration owns the context: every callback shares one,
	 * and freeing it with the first would leave the rest dangling. */
	ai_tool_executor_register_callback(executor, tool, venture_work_tool_grep,
	                                   ctx, venture_work_tools_ctx_free);
}
