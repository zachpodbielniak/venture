/*
 * test-docs.c - The documentation site: generator, checker, /docs, CLI
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of VENTURE.
 *
 * Issue #97. Every DONE WHEN in that issue has a test here that fails
 * without the code it names:
 *
 *   1. the generator renders every .org under docs/ and README.org to static
 *      HTML with a nav from docs/index.org, rewritten links and a search
 *      index -- test_generator_builtin, test_generator_emacs;
 *   2. a broken internal link or a doc not linked from index.org fails
 *      the build -- test_broken_link, test_unlinked_doc, test_cli_refuses;
 *   3. the quickstart is a linear path whose every command the suite
 *      runs against SQLite -- test_quickstart, which drives
 *      tests/docs-quickstart.sh;
 *   4. the running server serves the site at /docs without a session,
 *      and `venturectl docs build` renders it -- test_served,
 *      test_served_module_off, test_cli_build;
 *   5. README.org and docs/index.org point at the site and the quickstart
 *      -- test_entry_points.
 */

#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>
#include "venture-test-util.h"

/* ------------------------------------------------------------------------
 * Fixture trees
 * ---------------------------------------------------------------------- */

typedef struct
{
	gchar	*root;		/* a fake repository: README.org + docs/ */
	gchar	*out;		/* where the site is rendered */
} TreeFixture;

static void
write_file(
	const gchar	*root,
	const gchar	*relative,
	const gchar	*content
){
	g_autofree gchar *path = g_build_filename(root, relative, NULL);
	g_autofree gchar *dir = g_path_get_dirname(path);
	g_autoptr(GError) error = NULL;

	g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
	g_assert_true(g_file_set_contents(path, content, -1, &error));
	g_assert_no_error(error);
}

static gchar *
read_file(
	const gchar	*root,
	const gchar	*relative
){
	g_autofree gchar *path = g_build_filename(root, relative, NULL);
	g_autoptr(GError) error = NULL;
	gchar *content = NULL;

	g_assert_true(g_file_get_contents(path, &content, NULL, &error));
	g_assert_no_error(error);

	return content;
}

static gboolean
file_exists(
	const gchar	*root,
	const gchar	*relative
){
	g_autofree gchar *path = g_build_filename(root, relative, NULL);

	return g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

/*
 * A small but complete fake repository: an index with two groups, two
 * docs that link each other, a README that links into docs/, a link
 * with a heading search, and every org construct the builtin renderer
 * must handle.
 */
static void
tree_fixture_set_up(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	fixture->root = g_dir_make_tmp("venture-docs-XXXXXX", NULL);
	fixture->out = g_dir_make_tmp("venture-docs-out-XXXXXX", NULL);
	g_assert_nonnull(fixture->root);
	g_assert_nonnull(fixture->out);

	write_file(fixture->root, "README.org",
		"#+title: FAKE\n\n"
		"* FAKE\n\n"
		"See [[file:docs/index.org][the documentation]] and "
		"[[file:docs/quickstart.org][the quickstart]]. Built from "
		"[[file:Containerfile][Containerfile]].\n");
	write_file(fixture->root, "Containerfile", "FROM scratch\n");
	write_file(fixture->root, "docs/index.org",
		"#+title: FAKE documentation\n\n"
		"* FAKE\n\n"
		"An index.\n\n"
		"** Start here\n\n"
		"- [[file:quickstart.org][Quickstart]] --- zero to running\n"
		"- [[file:../README.org][README]] --- what it is\n\n"
		"** Documents\n\n"
		"- [[file:money.org][Money]] --- exact arithmetic\n");
	write_file(fixture->root, "docs/quickstart.org",
		"#+title: FAKE quickstart\n"
		"#+description: Build it, run it\n\n"
		"* Build\n\n"
		"Read [[file:money.org][money]] first, then "
		"[[file:money.org::*Allocation][allocation]], and "
		"[[https://example.test/x][the site]].\n\n"
		"#+begin_src sh\n"
		"make DEBUG=1 <tag>\n"
		"#+end_src\n\n"
		"** Steps\n\n"
		"- first =code & co=\n"
		"- second *bold* and /italic/\n"
		"  - nested ~tilde~\n"
		"- third\n\n\n"
		"1. one\n"
		"2. two\n\n"
		"| Column | Value |\n"
		"|--------+-------|\n"
		"| a      | 1     |\n"
		"| b      | 2     |\n\n"
		"#+begin_example\n"
		"  Password: secret\n"
		"#+end_example\n\n"
		"#+begin_quote\n"
		"A quote.\n"
		"#+end_quote\n\n"
		"Two paragraphs\nof one thought.\n\n"
		"Another paragraph.\n");
	write_file(fixture->root, "docs/money.org",
		"#+title: Money\n\n"
		"* Money\n\n"
		"Never a double. Back to [[file:quickstart.org][the quickstart]].\n\n"
		"** Allocation\n\n"
		"Sums back exactly.\n");
}

static void
tree_fixture_tear_down(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	venture_test_remove_tree(fixture->root);
	venture_test_remove_tree(fixture->out);
	g_clear_pointer(&fixture->root, g_free);
	g_clear_pointer(&fixture->out, g_free);
}

static gboolean
build_site(
	TreeFixture		 *fixture,
	VentureDocsRenderer	  renderer,
	GError			**error
){
	g_autoptr(VentureDocsSite) site = NULL;

	site = venture_docs_site_new(fixture->root, fixture->out);
	g_object_set(site, "renderer", renderer, NULL);

	return venture_docs_site_build(site, NULL, error);
}

/*
 * What every renderer must produce from the fixture tree. Shared by the
 * builtin and the Emacs test so the two cannot drift.
 */
static void
assert_site_shape(TreeFixture *fixture)
{
	g_autofree gchar *index = NULL;
	g_autofree gchar *quickstart = NULL;
	g_autofree gchar *readme = NULL;
	g_autofree gchar *search = NULL;
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(GError) error = NULL;
	JsonArray *entries;
	guint i;
	gboolean saw_quickstart = FALSE;

	/* Every org file, README included, became a page. */
	g_assert_true(file_exists(fixture->out, "index.html"));
	g_assert_true(file_exists(fixture->out, "quickstart.html"));
	g_assert_true(file_exists(fixture->out, "money.html"));
	g_assert_true(file_exists(fixture->out, "readme.html"));
	g_assert_true(file_exists(fixture->out, "venture-docs.css"));
	g_assert_true(file_exists(fixture->out, "venture-docs.js"));
	g_assert_true(file_exists(fixture->out, "search-index.json"));

	index = read_file(fixture->out, "index.html");
	quickstart = read_file(fixture->out, "quickstart.html");
	readme = read_file(fixture->out, "readme.html");

	/* The nav comes from index.org: its groups, in order, on every page. */
	g_assert_nonnull(strstr(index, "Start here"));
	g_assert_nonnull(strstr(quickstart, "Start here"));
	g_assert_nonnull(strstr(quickstart, "Documents"));
	g_assert_true(strstr(quickstart, "Start here") < strstr(quickstart, "Documents"));
	g_assert_nonnull(strstr(quickstart, "href=\"money.html\""));
	g_assert_nonnull(strstr(readme, "href=\"quickstart.html\""));
	/* The current page is marked in the nav. */
	g_assert_nonnull(strstr(quickstart, "aria-current=\"page\""));

	/* Links between docs were rewritten to the flat site, including
	 * the README's docs/ prefix and the index's ../README.org. */
	g_assert_nonnull(strstr(readme, "href=\"index.html\""));
	g_assert_nonnull(strstr(index, "href=\"readme.html\""));
	g_assert_null(strstr(readme, "docs/index"));
	g_assert_null(strstr(index, "../README"));
	g_assert_null(strstr(quickstart, ".org\""));
	/* A heading search becomes an anchor on the target page ... */
	g_assert_nonnull(strstr(quickstart, "href=\"money.html#allocation\""));
	/* ... which exists there. */
	{
		g_autofree gchar *money = read_file(fixture->out, "money.html");
		g_assert_nonnull(strstr(money, "id=\"allocation\""));
	}
	/* An existing non-org file is named, not linked into nowhere. */
	g_assert_null(strstr(readme, "href=\"Containerfile\""));
	g_assert_nonnull(strstr(readme, "Containerfile"));
	/* External links are left alone. */
	g_assert_nonnull(strstr(quickstart, "href=\"https://example.test/x\""));

	/* The page is a complete document with a title and the search box. */
	g_assert_nonnull(strstr(quickstart, "<!DOCTYPE html>"));
	g_assert_nonnull(strstr(quickstart, "<title>FAKE quickstart"));
	g_assert_nonnull(strstr(quickstart, "id=\"docs-search\""));
	g_assert_nonnull(strstr(quickstart, "venture-docs.js"));

	/* The body carries every construct: code, lists, tables, blocks. */
	g_assert_nonnull(strstr(quickstart, "make DEBUG=1 &lt;tag&gt;"));
	g_assert_nonnull(strstr(quickstart, "<table"));
	g_assert_nonnull(strstr(quickstart, "<th"));
	g_assert_nonnull(strstr(quickstart, "<ol"));
	g_assert_nonnull(strstr(quickstart, "<ul"));
	g_assert_nonnull(strstr(quickstart, "<blockquote"));
	g_assert_nonnull(strstr(quickstart, "Password: secret"));
	g_assert_nonnull(strstr(quickstart, "code &amp; co</code>"));
	g_assert_nonnull(strstr(quickstart, "<b>bold</b>"));
	g_assert_nonnull(strstr(quickstart, "<i>italic</i>"));
	g_assert_nonnull(strstr(quickstart, "<code>tilde</code>"));

	/* The search index is JSON naming every page with its text. */
	search = read_file(fixture->out, "search-index.json");
	g_assert_true(json_parser_load_from_data(parser, search, -1, &error));
	g_assert_no_error(error);
	entries = json_node_get_array(json_parser_get_root(parser));
	g_assert_cmpuint(json_array_get_length(entries), ==, 4);

	for (i = 0; i < json_array_get_length(entries); i++)
	{
		JsonObject *entry = json_array_get_object_element(entries, i);

		g_assert_true(json_object_has_member(entry, "url"));
		g_assert_true(json_object_has_member(entry, "title"));
		g_assert_true(json_object_has_member(entry, "text"));

		if (0 == g_strcmp0(json_object_get_string_member(entry, "url"),
		                   "quickstart.html"))
		{
			saw_quickstart = TRUE;
			g_assert_cmpstr(json_object_get_string_member(entry, "title"),
			                ==, "FAKE quickstart");
			/* Its own text, not another page's. */
			g_assert_null(strstr(
				json_object_get_string_member(entry, "text"),
				"Sums back"));
			g_assert_nonnull(strstr(
				json_object_get_string_member(entry, "text"),
				"Two paragraphs of one thought"));
			/* Tags are stripped from the searchable text, and the
			 * entities the renderer wrote are decoded back to text. */
			g_assert_null(strstr(
				json_object_get_string_member(entry, "text"), "<p>"));
			g_assert_null(strstr(
				json_object_get_string_member(entry, "text"), "<h2"));
			g_assert_null(strstr(
				json_object_get_string_member(entry, "text"), "<pre"));
			g_assert_nonnull(strstr(
				json_object_get_string_member(entry, "text"),
				"make DEBUG=1 <tag>"));
		}
	}

	g_assert_true(saw_quickstart);
}

/* ------------------------------------------------------------------------
 * 1. The generator
 * ---------------------------------------------------------------------- */

static void
test_generator_builtin(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDocsSite) site = NULL;

	site = venture_docs_site_new(fixture->root, fixture->out);
	g_object_set(site, "renderer", VENTURE_DOCS_RENDERER_BUILTIN, NULL);
	g_assert_true(venture_docs_site_build(site, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_docs_site_get_renderer_used(site), ==,
	                VENTURE_DOCS_RENDERER_BUILTIN);
	g_assert_cmpuint(venture_docs_site_get_page_count(site), ==, 4);

	assert_site_shape(fixture);
}

static void
test_generator_emacs(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDocsSite) site = NULL;
	g_autofree gchar *emacs = g_find_program_in_path("emacs");

	if (NULL == emacs)
	{
		g_test_skip("emacs is not on PATH");
		return;
	}

	site = venture_docs_site_new(fixture->root, fixture->out);
	g_object_set(site, "renderer", VENTURE_DOCS_RENDERER_EMACS, NULL);
	g_assert_true(venture_docs_site_build(site, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_docs_site_get_renderer_used(site), ==,
	                VENTURE_DOCS_RENDERER_EMACS);

	assert_site_shape(fixture);
}

/*
 * The automatic choice: Emacs when it is there, the builtin otherwise.
 * Which one is reported, so `make docs-site` can say which it used.
 */
static void
test_generator_auto(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDocsSite) site = NULL;
	g_autofree gchar *emacs = g_find_program_in_path("emacs");
	VentureDocsRenderer renderer;

	site = venture_docs_site_new(fixture->root, fixture->out);
	g_object_get(site, "renderer", &renderer, NULL);
	g_assert_cmpint(renderer, ==, VENTURE_DOCS_RENDERER_AUTO);
	g_assert_true(venture_docs_site_build(site, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_docs_site_get_renderer_used(site), ==,
	                (NULL != emacs) ? VENTURE_DOCS_RENDERER_EMACS
	                                : VENTURE_DOCS_RENDERER_BUILTIN);
}

/*
 * The real documentation renders, with the builtin renderer so this
 * does not depend on the machine: every .org under docs/ and README.org
 * becomes a page, the nav carries the Start here group, and nothing
 * is broken or orphaned -- which is the check `make docs-site` runs.
 */
static void
test_generator_real_docs(void)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureDocsSite) site = NULL;
	g_autofree gchar *out = g_dir_make_tmp("venture-docs-real-XXXXXX", NULL);
	g_autofree gchar *root = g_canonicalize_filename(".", NULL);
	g_autofree gchar *index = NULL;
	g_autofree gchar *quickstart = NULL;
	g_autoptr(GDir) dir = NULL;
	const gchar *name;
	guint org_files = 0;
	gboolean built;

	g_assert_nonnull(out);

	site = venture_docs_site_new(root, out);
	g_object_set(site, "renderer", VENTURE_DOCS_RENDERER_BUILTIN, NULL);
	built = venture_docs_site_build(site, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(built);

	dir = g_dir_open("docs", 0, &error);
	g_assert_no_error(error);

	while (NULL != (name = g_dir_read_name(dir)))
	{
		if (g_str_has_suffix(name, ".org"))
		{
			g_autofree gchar *page = g_strndup(name, strlen(name) - 4);
			g_autofree gchar *html = g_strconcat(page, ".html", NULL);

			org_files++;
			g_assert_true(file_exists(out, html));
		}
	}

	g_assert_true(file_exists(out, "readme.html"));
	g_assert_cmpuint(venture_docs_site_get_page_count(site), ==, org_files + 1);

	index = read_file(out, "index.html");
	g_assert_nonnull(strstr(index, "Start here"));
	g_assert_nonnull(strstr(index, "href=\"quickstart.html\""));
	g_assert_nonnull(strstr(index, "href=\"docs-site.html\""));

	quickstart = read_file(out, "quickstart.html");
	g_assert_nonnull(strstr(quickstart, "<title>VENTURE quickstart"));

	venture_test_remove_tree(out);
}

/* ------------------------------------------------------------------------
 * 2. The checker
 * ---------------------------------------------------------------------- */

static void
test_broken_link(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	write_file(fixture->root, "docs/money.org",
		"#+title: Money\n\n* Money\n\n"
		"See [[file:nowhere.org][a page that does not exist]].\n");

	g_assert_false(build_site(fixture, VENTURE_DOCS_RENDERER_BUILTIN, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DOCS_BROKEN_LINK);
	g_assert_nonnull(strstr(error->message, "money.org"));
	g_assert_nonnull(strstr(error->message, "nowhere.org"));

	/* A refused build leaves no half-rendered site behind. */
	g_assert_false(file_exists(fixture->out, "index.html"));
	g_assert_false(file_exists(fixture->out, "money.html"));
}

/* A link to a file outside the repository is broken too, not a way out. */
static void
test_broken_link_escapes(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	write_file(fixture->root, "docs/money.org",
		"#+title: Money\n\n* Money\n\n"
		"See [[file:../../../../etc/passwd][the machine]].\n");

	g_assert_false(build_site(fixture, VENTURE_DOCS_RENDERER_BUILTIN, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DOCS_BROKEN_LINK);
}

static void
test_unlinked_doc(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	write_file(fixture->root, "docs/orphan.org",
		"#+title: Orphan\n\n* Orphan\n\nNobody links here.\n");

	g_assert_false(build_site(fixture, VENTURE_DOCS_RENDERER_BUILTIN, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DOCS_UNLINKED);
	g_assert_nonnull(strstr(error->message, "orphan.org"));
	g_assert_nonnull(strstr(error->message, "index.org"));
	g_assert_false(file_exists(fixture->out, "index.html"));
}

/* Linked from another doc is not linked from the index. */
static void
test_unlinked_doc_indirect(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	write_file(fixture->root, "docs/orphan.org",
		"#+title: Orphan\n\n* Orphan\n\nOnly money links here.\n");
	write_file(fixture->root, "docs/money.org",
		"#+title: Money\n\n* Money\n\nSee [[file:orphan.org][orphan]].\n");

	g_assert_false(build_site(fixture, VENTURE_DOCS_RENDERER_BUILTIN, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_DOCS_UNLINKED);
	g_assert_nonnull(strstr(error->message, "orphan.org"));
}

/* No docs/index.org, no site: the nav has nothing to come from. */
static void
test_missing_index(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *index = g_build_filename(fixture->root, "docs", "index.org", NULL);

	g_assert_cmpint(g_unlink(index), ==, 0);
	g_assert_false(build_site(fixture, VENTURE_DOCS_RENDERER_BUILTIN, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_nonnull(strstr(error->message, "index.org"));
}

/* ------------------------------------------------------------------------
 * 4a. The CLI
 * ---------------------------------------------------------------------- */

static gint
run_ctl(
	const gchar	*const	*argv,
	gchar		      **out,
	gchar		      **err
){
	g_autoptr(GError) error = NULL;
	gint status = -1;

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, NULL,
		G_SPAWN_DEFAULT, NULL, NULL, out, err, &status, &error));
	g_assert_no_error(error);

	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void
test_cli_build(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cli = g_canonicalize_filename("build/debug/venturectl", NULL);
	g_autofree gchar *source = g_strconcat("source=", fixture->root, NULL);
	g_autofree gchar *output = g_strconcat("output=", fixture->out, NULL);
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	const gchar *argv[] = { NULL, "docs", "build", NULL, NULL, "renderer=builtin", NULL };

	if (!g_file_test(cli, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("build/debug/venturectl is not built");
		return;
	}

	argv[0] = cli;
	argv[3] = source;
	argv[4] = output;

	g_assert_cmpint(run_ctl(argv, &out, &err), ==, 0);
	/* It says what it rendered, how many, and with which renderer. */
	g_assert_nonnull(strstr(out, "4 pages"));
	g_assert_nonnull(strstr(out, "builtin"));
	g_assert_nonnull(strstr(out, fixture->out));
	g_assert_true(file_exists(fixture->out, "quickstart.html"));
	g_assert_true(file_exists(fixture->out, "search-index.json"));
}

static void
test_cli_refuses(
	TreeFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *cli = g_canonicalize_filename("build/debug/venturectl", NULL);
	g_autofree gchar *source = g_strconcat("source=", fixture->root, NULL);
	g_autofree gchar *output = g_strconcat("output=", fixture->out, NULL);
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	const gchar *argv[] = { NULL, "docs", "build", NULL, NULL, "renderer=builtin", NULL };
	const gchar *bad_verb[] = { NULL, "docs", "publish", NULL };
	const gchar *bad_renderer[] = { NULL, "docs", "build", NULL, NULL, "renderer=pandoc", NULL };

	if (!g_file_test(cli, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("build/debug/venturectl is not built");
		return;
	}

	argv[0] = cli;
	argv[3] = source;
	argv[4] = output;
	bad_verb[0] = cli;
	bad_renderer[0] = cli;
	bad_renderer[3] = source;
	bad_renderer[4] = output;

	write_file(fixture->root, "docs/money.org",
		"#+title: Money\n\n* Money\n\nSee [[file:nowhere.org][gone]].\n");

	/* A broken link is a failed command that names the link, exit 8. */
	g_assert_cmpint(run_ctl(argv, &out, &err), ==, 8);
	g_assert_nonnull(strstr(err, "nowhere.org"));
	g_assert_false(file_exists(fixture->out, "index.html"));

	g_clear_pointer(&out, g_free);
	g_clear_pointer(&err, g_free);
	g_assert_cmpint(run_ctl(bad_verb, &out, &err), ==, 2);
	g_assert_nonnull(strstr(err, "usage: venturectl docs build"));

	g_clear_pointer(&out, g_free);
	g_clear_pointer(&err, g_free);
	g_assert_cmpint(run_ctl(bad_renderer, &out, &err), ==, 2);
	g_assert_nonnull(strstr(err, "pandoc"));
}

/* ------------------------------------------------------------------------
 * 4b. /docs on the running server
 * ---------------------------------------------------------------------- */

typedef struct
{
	TreeFixture		 tree;
	VentureDatabase		*database;
	VentureConfig		*config;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	guint16			 port;
	gchar			*state_dir;
} ServerFixture;

static void
server_fixture_start(
	ServerFixture	*fixture,
	gboolean	 docs_module
){
	g_autoptr(GError) error = NULL;

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
	             "security-require-auth", TRUE,
	             "docs-site-dir", fixture->tree.out,
	             NULL);
	venture_config_set_module_enabled(fixture->config, "docs", docs_module);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);

	fixture->session = soup_session_new_with_options("timeout", 15, NULL);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	tree_fixture_set_up(&fixture->tree, NULL);
	g_assert_true(build_site(&fixture->tree, VENTURE_DOCS_RENDERER_BUILTIN, &error));
	g_assert_no_error(error);

	fixture->state_dir = g_dir_make_tmp("venture-docs-srv-XXXXXX", NULL);
	fixture->port = (guint16)(20000 + ((getpid() + 7) % 20000));

	server_fixture_start(fixture, GPOINTER_TO_INT(user_data));
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	venture_test_remove_tree(fixture->state_dir);
	g_clear_pointer(&fixture->state_dir, g_free);
	tree_fixture_tear_down(&fixture->tree, NULL);
}

typedef struct
{
	gboolean	 done;
	GBytes		*bytes;
	GError		*error;
} FetchResult;

static void
fetch_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 data
){
	FetchResult *fetched = data;

	fetched->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                   result, &fetched->error);
	fetched->done = TRUE;
}

/*
 * A GET with no session and no token. Returns the status; @body and
 * @content_type, when asked for, carry what came back. Redirects are
 * not followed: where a page sends an anonymous caller is the point.
 * The server runs on this thread's main context, so the request is
 * asynchronous and the loop is iterated until it completes.
 */
static guint
fetch(
	ServerFixture	 *fixture,
	const gchar	 *path,
	gchar		**body,
	gchar		**content_type,
	gchar		**location
){
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	g_autoptr(SoupMessage) message = soup_message_new("GET", url);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	FetchResult result = { FALSE, NULL, NULL };
	SoupMessageHeaders *headers;

	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(fixture->session, message, G_PRIORITY_DEFAULT,
	                                 NULL, fetch_done, &result);

	while (!result.done)
		g_main_context_iteration(NULL, TRUE);

	error = result.error;
	bytes = result.bytes;
	g_assert_no_error(error);

	headers = soup_message_get_response_headers(message);

	if (NULL != body)
	{
		gsize size = 0;
		const gchar *data = g_bytes_get_data(bytes, &size);

		*body = g_strndup(data, size);
	}

	if (NULL != content_type)
		*content_type = g_strdup(soup_message_headers_get_content_type(headers, NULL));

	if (NULL != location)
		*location = g_strdup(soup_message_headers_get_one(headers, "Location"));

	return soup_message_get_status(message);
}

static void
test_served(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *body = NULL;
	g_autofree gchar *type = NULL;
	g_autofree gchar *location = NULL;

	/* The site's front door, without a session. */
	g_assert_cmpuint(fetch(fixture, "/docs", NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/docs/index.html");
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(fetch(fixture, "/docs/", NULL, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/docs/index.html");

	g_assert_cmpuint(fetch(fixture, "/docs/index.html", &body, &type, NULL), ==, 200);
	g_assert_cmpstr(type, ==, "text/html");
	g_assert_nonnull(strstr(body, "Start here"));
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&type, g_free);

	g_assert_cmpuint(fetch(fixture, "/docs/quickstart.html", &body, &type, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "FAKE quickstart"));
	g_clear_pointer(&body, g_free);
	g_clear_pointer(&type, g_free);

	/* Its assets, with the right types. */
	g_assert_cmpuint(fetch(fixture, "/docs/venture-docs.css", NULL, &type, NULL), ==, 200);
	g_assert_cmpstr(type, ==, "text/css");
	g_clear_pointer(&type, g_free);
	g_assert_cmpuint(fetch(fixture, "/docs/venture-docs.js", NULL, &type, NULL), ==, 200);
	g_assert_cmpstr(type, ==, "application/javascript");
	g_clear_pointer(&type, g_free);
	g_assert_cmpuint(fetch(fixture, "/docs/search-index.json", NULL, &type, NULL), ==, 200);
	g_assert_cmpstr(type, ==, "application/json");
	g_clear_pointer(&type, g_free);

	/* Read-only, and only what the generator wrote. */
	g_assert_cmpuint(fetch(fixture, "/docs/nowhere.html", NULL, NULL, NULL), ==, 404);
	/* An encoded traversal is refused by the router (400) before the
	 * route sees it; either way it is not a file. */
	{
		guint status = fetch(fixture, "/docs/..%2F..%2Fetc%2Fpasswd", NULL, NULL, NULL);

		g_assert_true(400 == status || 404 == status);
	}
	g_assert_cmpuint(fetch(fixture, "/docs/..%2Fventure-docs.css", NULL, NULL, NULL), !=, 200);
	g_assert_cmpuint(fetch(fixture, "/docs/index.org", NULL, NULL, NULL), ==, 404);
	/* A path with a directory in it matches no docs route at all and
	 * falls through to the rest of the server, which wants a session. */
	g_assert_cmpuint(fetch(fixture, "/docs/sub/index.html", NULL, NULL, NULL), !=, 200);

	/* The rest of the server still wants a session. */
	g_assert_cmpuint(fetch(fixture, "/", NULL, NULL, &location), ==, 302);
	g_assert_nonnull(strstr(location, "/login"));
}

/* An instance whose site was never rendered says so, rather than 500. */
static void
test_served_not_built(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autofree gchar *body = NULL;
	g_autofree gchar *index = g_build_filename(fixture->tree.out, "index.html", NULL);

	g_assert_cmpint(g_unlink(index), ==, 0);
	g_assert_cmpuint(fetch(fixture, "/docs/index.html", &body, NULL, NULL), ==, 404);
	g_assert_nonnull(strstr(body, "make docs-site"));
}

/* The docs module is a switch like every other. */
static void
test_served_module_off(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_assert_cmpuint(fetch(fixture, "/docs", NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(fetch(fixture, "/docs/index.html", NULL, NULL, NULL), ==, 404);
	g_assert_cmpuint(fetch(fixture, "/docs/venture-docs.css", NULL, NULL, NULL), ==, 404);
}

/* ------------------------------------------------------------------------
 * 3. The quickstart, run for real
 * ---------------------------------------------------------------------- */

/*
 * tests/docs-quickstart.sh extracts every named shell block from
 * docs/quickstart.org, runs them in order against a throwaway SQLite
 * instance on a private port, and asserts each step's result. Its exit
 * status is the test; its output is the diagnosis when it fails.
 *
 * It needs both binaries and the demo script, so the suite builds
 * venturectl before this binary (Makefile) and the script is skipped,
 * not failed, when the tree is not built.
 */
static void
test_quickstart(void)
{
	g_autofree gchar *script = g_canonicalize_filename("tests/docs-quickstart.sh", NULL);
	g_autofree gchar *cli = g_canonicalize_filename("build/debug/venturectl", NULL);
	g_autofree gchar *server = g_canonicalize_filename("build/debug/venture", NULL);
	g_autofree gchar *out = NULL;
	g_autofree gchar *err = NULL;
	g_autofree gchar *port = g_strdup_printf("%u", 20000 + ((getpid() + 11) % 20000));
	g_autofree gchar *state = g_dir_make_tmp("venture-quickstart-XXXXXX", NULL);
	g_autoptr(GError) error = NULL;
	const gchar *argv[] = { "bash", NULL, NULL };
	gchar **envp;
	gint status = -1;

	if (!g_file_test(cli, G_FILE_TEST_IS_EXECUTABLE) ||
	    !g_file_test(server, G_FILE_TEST_IS_EXECUTABLE))
	{
		g_test_skip("build/debug binaries are not built");
		venture_test_remove_tree(state);
		return;
	}

	argv[1] = script;
	envp = g_get_environ();
	envp = g_environ_setenv(envp, "PORT", port, TRUE);
	envp = g_environ_setenv(envp, "VENTURE_DEMO_STATE", state, TRUE);
	envp = g_environ_setenv(envp, "BUILD_TYPE", "debug", TRUE);

	g_assert_true(g_spawn_sync(NULL, (gchar **)argv, envp, G_SPAWN_SEARCH_PATH,
		NULL, NULL, &out, &err, &status, &error));
	g_assert_no_error(error);
	g_strfreev(envp);

	if (!WIFEXITED(status) || 0 != WEXITSTATUS(status))
	{
		g_test_message("docs-quickstart.sh stdout:\n%s", out);
		g_test_message("docs-quickstart.sh stderr:\n%s", err);
	}

	g_assert_true(WIFEXITED(status));
	g_assert_cmpint(WEXITSTATUS(status), ==, 0);
	/* Every step reported, in the order the quickstart tells them. */
	g_assert_nonnull(strstr(out, "ok qs-seed"));
	g_assert_nonnull(strstr(out, "ok qs-home"));
	g_assert_nonnull(strstr(out, "ok qs-invoice"));
	g_assert_nonnull(strstr(out, "ok qs-bank"));
	g_assert_nonnull(strstr(out, "ok qs-close"));
	g_assert_true(strstr(out, "ok qs-seed") < strstr(out, "ok qs-home"));
	g_assert_true(strstr(out, "ok qs-invoice") < strstr(out, "ok qs-bank"));
	g_assert_true(strstr(out, "ok qs-bank") < strstr(out, "ok qs-close"));

	venture_test_remove_tree(state);
}

/* ------------------------------------------------------------------------
 * 5. Entry points
 * ---------------------------------------------------------------------- */

static void
test_entry_points(void)
{
	g_autofree gchar *readme = read_file(".", "README.org");
	g_autofree gchar *index = read_file(".", "docs/index.org");
	const gchar *start;

	/* README points at the site and the quickstart. */
	g_assert_nonnull(strstr(readme, "make docs-site"));
	g_assert_nonnull(strstr(readme, "/docs"));
	g_assert_nonnull(strstr(readme, "[[file:docs/quickstart.org]"));

	/* index.org has a Start here section, before the document list,
	 * pointing at the quickstart and the site's own page. */
	start = strstr(index, "* Start here");
	g_assert_nonnull(start);
	g_assert_true(start < strstr(index, "** Documents"));
	g_assert_nonnull(strstr(start, "[[file:quickstart.org]"));
	g_assert_nonnull(strstr(start, "[[file:docs-site.org]"));
}

/* ------------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add("/docs/generator/builtin", TreeFixture, NULL,
	           tree_fixture_set_up, test_generator_builtin, tree_fixture_tear_down);
	g_test_add("/docs/generator/emacs", TreeFixture, NULL,
	           tree_fixture_set_up, test_generator_emacs, tree_fixture_tear_down);
	g_test_add("/docs/generator/auto", TreeFixture, NULL,
	           tree_fixture_set_up, test_generator_auto, tree_fixture_tear_down);
	g_test_add_func("/docs/generator/real-docs", test_generator_real_docs);

	g_test_add("/docs/checker/broken-link", TreeFixture, NULL,
	           tree_fixture_set_up, test_broken_link, tree_fixture_tear_down);
	g_test_add("/docs/checker/broken-link-escapes", TreeFixture, NULL,
	           tree_fixture_set_up, test_broken_link_escapes, tree_fixture_tear_down);
	g_test_add("/docs/checker/unlinked", TreeFixture, NULL,
	           tree_fixture_set_up, test_unlinked_doc, tree_fixture_tear_down);
	g_test_add("/docs/checker/unlinked-indirect", TreeFixture, NULL,
	           tree_fixture_set_up, test_unlinked_doc_indirect, tree_fixture_tear_down);
	g_test_add("/docs/checker/missing-index", TreeFixture, NULL,
	           tree_fixture_set_up, test_missing_index, tree_fixture_tear_down);

	g_test_add("/docs/cli/build", TreeFixture, NULL,
	           tree_fixture_set_up, test_cli_build, tree_fixture_tear_down);
	g_test_add("/docs/cli/refuses", TreeFixture, NULL,
	           tree_fixture_set_up, test_cli_refuses, tree_fixture_tear_down);

	g_test_add("/docs/served/site", ServerFixture, GINT_TO_POINTER(TRUE),
	           server_fixture_set_up, test_served, server_fixture_tear_down);
	g_test_add("/docs/served/not-built", ServerFixture, GINT_TO_POINTER(TRUE),
	           server_fixture_set_up, test_served_not_built, server_fixture_tear_down);
	g_test_add("/docs/served/module-off", ServerFixture, GINT_TO_POINTER(FALSE),
	           server_fixture_set_up, test_served_module_off, server_fixture_tear_down);

	g_test_add_func("/docs/entry-points", test_entry_points);
	g_test_add_func("/docs/quickstart", test_quickstart);

	return g_test_run();
}
