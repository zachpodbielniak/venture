/*
 * test-work-sandbox.c - The boundary the in-process runner rests on
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The in-process coding runner executes inside the server process, with the
 * server's uid and the server's environment. What stops an agent reading the
 * database, the configuration or /proc/self/environ -- where the session
 * secret and every provider key live -- is one function. This file is that
 * function's test, and every case in it is a way path confinement has been
 * got wrong somewhere before.
 */

#include <venture.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "venture-test-util.h"

typedef struct
{
	gchar *base;
	gchar *root;
	gchar *outside;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *inside = NULL;

	(void)user_data;

	/*
	 * Kept on the fixture rather than freed here. The temp root is the
	 * only handle on both `workspace` and `elsewhere`, and dropping it
	 * left the teardown with nothing to remove -- seven
	 * /tmp/venture-sandbox-* directories per run, each holding two
	 * subdirectories and two files.
	 */
	fixture->base = g_dir_make_tmp("venture-sandbox-XXXXXX", &error);
	g_assert_no_error(error);

	fixture->root = g_build_filename(fixture->base, "workspace", NULL);
	fixture->outside = g_build_filename(fixture->base, "elsewhere", NULL);

	g_assert_cmpint(g_mkdir_with_parents(fixture->root, 0755), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(fixture->outside, 0755), ==, 0);

	inside = g_build_filename(fixture->root, "hello.txt", NULL);
	g_assert_true(g_file_set_contents(inside, "inside\n", -1, &error));
	g_assert_no_error(error);

	{
		g_autofree gchar *secret = NULL;

		secret = g_build_filename(fixture->outside, "secret.txt", NULL);
		g_assert_true(g_file_set_contents(secret, "do not read me\n", -1,
		                                  &error));
		g_assert_no_error(error);
	}
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	/*
	 * The base, which holds both of the others -- and this fixture is
	 * the one that deliberately creates a path *outside* the sandbox
	 * root, so removing `root` alone would leave `elsewhere` and its
	 * secret.txt behind.
	 */
	venture_test_remove_tree(fixture->base);

	g_clear_pointer(&fixture->base, g_free);
	g_clear_pointer(&fixture->root, g_free);
	g_clear_pointer(&fixture->outside, g_free);
}

/*
 * Paths inside the workspace resolve, including the root itself.
 *
 * Without this the rest of the file would pass against a resolver that
 * refused everything.
 */
static void
test_sandbox_allows_paths_inside(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *relative = NULL;
	g_autofree gchar *absolute = NULL;
	g_autofree gchar *nested = NULL;
	g_autofree gchar *itself = NULL;
	g_autofree gchar *absolute_input = NULL;

	(void)user_data;

	relative = venture_work_tools_resolve(fixture->root, "hello.txt", TRUE,
	                                      &error);
	g_assert_no_error(error);
	g_assert_nonnull(relative);

	/* The agent is told the workspace path, so an absolute reference to a
	 * file inside it is reasonable and must work. */
	absolute_input = g_build_filename(fixture->root, "hello.txt", NULL);
	absolute = venture_work_tools_resolve(fixture->root, absolute_input, TRUE,
	                                      &error);
	g_assert_no_error(error);
	g_assert_nonnull(absolute);

	/*
	 * A file that does not exist yet, in a directory that does: the
	 * parent is what gets canonicalised.
	 */
	{
		g_autofree gchar *subdir = NULL;

		subdir = g_build_filename(fixture->root, "src", NULL);
		g_assert_cmpint(g_mkdir_with_parents(subdir, 0755), ==, 0);
	}

	nested = venture_work_tools_resolve(fixture->root, "src/new.c", FALSE,
	                                    &error);
	g_assert_no_error(error);
	g_assert_nonnull(nested);

	/* A parent that does not exist is refused rather than invented: the
	 * write tool creates directories deliberately, after the check. */
	{
		g_autoptr(GError) missing = NULL;
		g_autofree gchar *deep = NULL;

		deep = venture_work_tools_resolve(fixture->root, "no/such/dir/f.c",
		                                  FALSE, &missing);
		g_assert_null(deep);
		g_assert_nonnull(missing);
	}

	itself = venture_work_tools_resolve(fixture->root, ".", TRUE, &error);
	g_assert_no_error(error);
	g_assert_nonnull(itself);
}

/*
 * Every way out of the workspace is refused.
 *
 * What breaks if this regresses: ai-glib's own resolver returns an absolute
 * path unchanged and never normalises "..", which is precisely why these
 * tools exist instead of its built-ins. Each entry below is one of those
 * two mistakes.
 */
static void
test_sandbox_refuses_paths_outside(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *absolute_outside = NULL;
	static const gchar *const escapes[] = {
		"../elsewhere/secret.txt",
		"../../etc/passwd",
		"./../../etc/passwd",
		"subdir/../../elsewhere/secret.txt",
		"/etc/passwd",
		"/proc/self/environ"
	};
	gsize i;

	(void)user_data;

	for (i = 0; i < G_N_ELEMENTS(escapes); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *resolved = NULL;

		resolved = venture_work_tools_resolve(fixture->root, escapes[i], TRUE,
		                                      &error);

		g_assert_null(resolved);
		g_assert_nonnull(error);
	}

	/* And the absolute path to the sibling directory, spelled out. */
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *resolved = NULL;

		absolute_outside = g_build_filename(fixture->outside, "secret.txt",
		                                    NULL);
		resolved = venture_work_tools_resolve(fixture->root, absolute_outside,
		                                      TRUE, &error);
		g_assert_null(resolved);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	}
}

/*
 * A sibling directory whose name merely starts with the workspace's is not
 * inside it.
 *
 * What breaks if this regresses: a plain g_str_has_prefix check passes
 * "/tmp/x/workspace-evil" for a root of "/tmp/x/workspace". The agent can
 * create that directory itself, so this is not a hypothetical -- it is the
 * shortest route out of the sandbox, and it is the single most commonly
 * botched line in any containment check.
 */
static void
test_sandbox_refuses_a_sibling_with_a_shared_prefix(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *evil_dir = NULL;
	g_autofree gchar *evil_file = NULL;
	g_autofree gchar *resolved = NULL;

	(void)user_data;

	evil_dir = g_strconcat(fixture->root, "-evil", NULL);
	g_assert_cmpint(g_mkdir_with_parents(evil_dir, 0755), ==, 0);

	evil_file = g_build_filename(evil_dir, "loot.txt", NULL);
	g_assert_true(g_file_set_contents(evil_file, "loot\n", -1, &error));
	g_assert_no_error(error);

	resolved = venture_work_tools_resolve(fixture->root, evil_file, TRUE,
	                                      &error);

	g_assert_null(resolved);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

/*
 * A symlink out of the workspace is refused, including one made after the
 * run started.
 *
 * What breaks if this regresses: resolution has to happen at every use, not
 * once when the workspace is prepared. An agent that can write a file can
 * write a symlink, so a check that trusted the tree as it was found would be
 * defeated by the agent's own first action.
 */
static void
test_sandbox_refuses_a_symlink_out(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *link_path = NULL;
	g_autofree gchar *target = NULL;
	g_autofree gchar *resolved = NULL;
	g_autofree gchar *dir_link = NULL;
	g_autofree gchar *through_dir = NULL;

	(void)user_data;

	target = g_build_filename(fixture->outside, "secret.txt", NULL);
	link_path = g_build_filename(fixture->root, "escape.txt", NULL);

	if (0 != symlink(target, link_path))
	{
		g_test_skip("this filesystem does not do symlinks");
		return;
	}

	resolved = venture_work_tools_resolve(fixture->root, "escape.txt", TRUE,
	                                      &error);
	g_assert_null(resolved);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);

	/* And a symlinked directory walked through, rather than named. */
	dir_link = g_build_filename(fixture->root, "out", NULL);
	g_assert_cmpint(symlink(fixture->outside, dir_link), ==, 0);

	through_dir = venture_work_tools_resolve(fixture->root, "out/secret.txt",
	                                         TRUE, &error);
	g_assert_null(through_dir);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
}

/*
 * A file being created for the first time resolves by its parent, and the
 * parent still has to be inside.
 *
 * What breaks if this regresses: realpath() fails on a path whose final
 * component does not exist, so a naive resolver refuses every write of a new
 * file -- or, worse, skips the check entirely for exactly those writes,
 * which is the case that lets an agent create a file anywhere it likes.
 */
static void
test_sandbox_handles_a_file_that_does_not_exist_yet(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *inside = NULL;
	g_autofree gchar *outside = NULL;

	(void)user_data;

	inside = venture_work_tools_resolve(fixture->root, "brand-new.txt", FALSE,
	                                    &error);
	g_assert_no_error(error);
	g_assert_nonnull(inside);

	outside = venture_work_tools_resolve(fixture->root,
	                                     "../elsewhere/brand-new.txt", FALSE,
	                                     &error);
	g_assert_null(outside);
	g_assert_nonnull(error);
}

/*
 * The executor these tools are registered on offers no shell.
 *
 * What breaks if this regresses: CLAUDE.md's rule is that VENTURE's executor
 * is built empty, and the reason is that a bash tool walks around every
 * confinement in this file in one call. ai-glib runs a built-in only when
 * the executor advertises it, so an empty executor genuinely has no bash --
 * asserted here by calling it, not by reading the tool list, because the
 * tool list is not what the dispatcher consults.
 */
static void
test_sandbox_executor_has_no_shell(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(AiToolExecutor) executor = NULL;
	g_autoptr(AiToolUse) tool_use = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *result = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) input = NULL;

	(void)user_data;

	executor = ai_tool_executor_new_empty();
	venture_work_tools_register(executor, fixture->root);

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "command");
	json_builder_add_string_value(builder, "cat /etc/passwd");
	json_builder_end_object(builder);
	input = json_builder_get_root(builder);

	tool_use = ai_tool_use_new("call-1", "bash", input);

	result = ai_tool_executor_execute(executor, tool_use, NULL, &error);

	g_assert_null(result);
	g_assert_nonnull(error);
}

/*
 * The registered tools work for ordinary use, and refuse an escape.
 *
 * End to end through the executor rather than through the resolver, because
 * a tool that forgot to call the resolver would pass every test above.
 */
static void
test_sandbox_tools_confine_their_arguments(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(AiToolExecutor) executor = NULL;
	g_autofree gchar *inside = NULL;
	g_autofree gchar *outside = NULL;
	g_autofree gchar *written = NULL;

	(void)user_data;

	executor = ai_tool_executor_new_empty();
	venture_work_tools_register(executor, fixture->root);

#define RUN(tool, key1, value1, key2, value2)                                 \
	({                                                                    \
		g_autoptr(JsonBuilder) b = json_builder_new();                \
		g_autoptr(JsonNode) n = NULL;                                 \
		g_autoptr(AiToolUse) use = NULL;                              \
		json_builder_begin_object(b);                                 \
		json_builder_set_member_name(b, key1);                        \
		json_builder_add_string_value(b, value1);                     \
		if (NULL != (key2))                                           \
		{                                                             \
			json_builder_set_member_name(b, key2);                \
			json_builder_add_string_value(b, value2);             \
		}                                                             \
		json_builder_end_object(b);                                   \
		n = json_builder_get_root(b);                                 \
		use = ai_tool_use_new("c", tool, n);                          \
		ai_tool_executor_execute(executor, use, NULL, NULL);          \
	})

	inside = RUN("read", "path", "hello.txt", NULL, NULL);
	g_assert_nonnull(inside);
	g_assert_cmpstr(inside, ==, "inside\n");

	outside = RUN("read", "path", "../elsewhere/secret.txt", NULL, NULL);
	g_assert_nonnull(outside);
	g_assert_true(g_str_has_prefix(outside, "Error:"));
	g_assert_null(strstr(outside, "do not read me"));

	written = RUN("write", "path", "../elsewhere/planted.txt",
	              "contents", "should not land");
	g_assert_nonnull(written);
	g_assert_true(g_str_has_prefix(written, "Error:"));

	{
		g_autofree gchar *planted = NULL;

		planted = g_build_filename(fixture->outside, "planted.txt", NULL);
		g_assert_false(g_file_test(planted, G_FILE_TEST_EXISTS));
	}

#undef RUN
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/work-sandbox/allows-paths-inside", test_sandbox_allows_paths_inside);
	ADD("/work-sandbox/refuses-paths-outside",
	    test_sandbox_refuses_paths_outside);
	ADD("/work-sandbox/refuses-a-sibling-with-a-shared-prefix",
	    test_sandbox_refuses_a_sibling_with_a_shared_prefix);
	ADD("/work-sandbox/refuses-a-symlink-out",
	    test_sandbox_refuses_a_symlink_out);
	ADD("/work-sandbox/handles-a-file-that-does-not-exist-yet",
	    test_sandbox_handles_a_file_that_does_not_exist_yet);
	ADD("/work-sandbox/executor-has-no-shell",
	    test_sandbox_executor_has_no_shell);
	ADD("/work-sandbox/tools-confine-their-arguments",
	    test_sandbox_tools_confine_their_arguments);

#undef ADD

	return g_test_run();
}
