/*
 * venture-docs-site.c - The documentation site, rendered from every .org under docs/
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of VENTURE.
 *
 * A build is four passes, and nothing is written until the third:
 *
 *   1. collect: every .org under docs/ and README.org under the source tree;
 *   2. resolve: every [[file:...]] link in every page is rewritten to the
 *      flat site -- docs/money.org becomes money.html, a "::*Heading"
 *      search becomes an anchor -- and a target that does not exist is
 *      the build's failure. The same pass records what docs/index.org
 *      links, and a page it does not link is the other failure;
 *   3. render: the rewritten org becomes an HTML body, through Emacs's
 *      exporter in batch or the builtin renderer;
 *   4. write: each body wrapped in the page shell with the nav that
 *      index.org describes, plus the stylesheet, the script and the
 *      search index.
 *
 * Refusing before writing is what lets `make docs-site` be trusted: a
 * failed build leaves no site behind for the server to serve.
 */

#include "venture.h"
#include "venture-docs-assets.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

/* ------------------------------------------------------------------------
 * The renderer enum
 * ---------------------------------------------------------------------- */

static const GEnumValue venture_docs_renderer_values[] = {
	{ VENTURE_DOCS_RENDERER_AUTO, "VENTURE_DOCS_RENDERER_AUTO", "auto" },
	{ VENTURE_DOCS_RENDERER_EMACS, "VENTURE_DOCS_RENDERER_EMACS", "emacs" },
	{ VENTURE_DOCS_RENDERER_BUILTIN, "VENTURE_DOCS_RENDERER_BUILTIN", "builtin" },
	{ 0, NULL, NULL }
};

GType
venture_docs_renderer_get_type(void)
{
	static gsize type_id = 0;

	if (g_once_init_enter(&type_id))
	{
		GType id = g_enum_register_static("VentureDocsRenderer",
		                                  venture_docs_renderer_values);
		g_once_init_leave(&type_id, id);
	}

	return type_id;
}

/**
 * venture_docs_renderer_to_nick:
 * @renderer: a renderer
 *
 * Returns: the renderer's short name: "auto", "emacs" or "builtin"
 */
const gchar *
venture_docs_renderer_to_nick(VentureDocsRenderer renderer)
{
	gsize i;

	for (i = 0; NULL != venture_docs_renderer_values[i].value_nick; i++)
	{
		if ((gint)renderer == venture_docs_renderer_values[i].value)
			return venture_docs_renderer_values[i].value_nick;
	}

	return "auto";
}

/**
 * venture_docs_renderer_from_nick:
 * @nick: a short name
 * @renderer: (out): the renderer it names
 *
 * Returns: %TRUE if @nick names a renderer
 */
gboolean
venture_docs_renderer_from_nick(
	const gchar		*nick,
	VentureDocsRenderer	*renderer
){
	gsize i;

	g_return_val_if_fail(NULL != renderer, FALSE);

	if (NULL == nick)
		return FALSE;

	for (i = 0; NULL != venture_docs_renderer_values[i].value_nick; i++)
	{
		if (0 == g_strcmp0(nick, venture_docs_renderer_values[i].value_nick))
		{
			*renderer = (VentureDocsRenderer)venture_docs_renderer_values[i].value;
			return TRUE;
		}
	}

	return FALSE;
}

/* ------------------------------------------------------------------------
 * The object
 * ---------------------------------------------------------------------- */

struct _VentureDocsSite
{
	GObject parent_instance;

	gchar			*source_dir;
	gchar			*output_dir;
	VentureDocsRenderer	 renderer;

	/* What the last build did. */
	VentureDocsRenderer	 renderer_used;
	guint			 page_count;
};

G_DEFINE_FINAL_TYPE(VentureDocsSite, venture_docs_site, G_TYPE_OBJECT)

enum
{
	PROP_0,
	PROP_SOURCE_DIR,
	PROP_OUTPUT_DIR,
	PROP_RENDERER,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

/*
 * One document on its way to being a page.
 */
typedef struct
{
	gchar		*path;		/* absolute source path */
	gchar		*relative;	/* "docs/money.org", "README.org" */
	gchar		*slug;		/* "money", "readme" */
	gchar		*title;
	gchar		*description;
	gchar		*org;		/* the source, links rewritten */
	gchar		*body;		/* rendered HTML */
} Page;

static void
page_free(Page *page)
{
	g_free(page->path);
	g_free(page->relative);
	g_free(page->slug);
	g_free(page->title);
	g_free(page->description);
	g_free(page->org);
	g_free(page->body);
	g_free(page);
}

/* A nav entry, read from a list item in docs/index.org. */
typedef struct
{
	gchar	*slug;
	gchar	*title;
} NavEntry;

static void
nav_entry_free(NavEntry *entry)
{
	g_free(entry->slug);
	g_free(entry->title);
	g_free(entry);
}

typedef struct
{
	gchar		*title;
	GPtrArray	*entries;	/* of NavEntry */
} NavGroup;

static void
nav_group_free(NavGroup *group)
{
	g_free(group->title);
	g_ptr_array_unref(group->entries);
	g_free(group);
}

/* ------------------------------------------------------------------------
 * 1. Collect
 * ---------------------------------------------------------------------- */

static gchar *
slug_for(const gchar *filename)
{
	g_autofree gchar *base = g_path_get_basename(filename);
	gchar *slug;

	if (g_str_has_suffix(base, ".org"))
		base[strlen(base) - 4] = '\0';

	slug = g_ascii_strdown(base, -1);

	return slug;
}

static Page *
page_load(
	const gchar	 *source_dir,
	const gchar	 *relative,
	GError		**error
){
	g_autofree gchar *path = g_build_filename(source_dir, relative, NULL);
	g_autofree gchar *org = NULL;
	Page *page;

	if (!g_file_get_contents(path, &org, NULL, error))
		return NULL;

	page = g_new0(Page, 1);
	page->path = g_steal_pointer(&path);
	page->relative = g_strdup(relative);
	page->slug = slug_for(relative);
	page->title = venture_org_get_keyword(org, "title");
	page->description = venture_org_get_keyword(org, "description");
	page->org = g_steal_pointer(&org);

	if (NULL == page->title)
		page->title = g_strdup(page->slug);

	return page;
}

static gint
compare_names(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

/*
 * Every .org under docs/ in name order, then README.org. Returns pages keyed
 * by slug as well, because the resolver looks them up by name.
 */
static GPtrArray *
collect_pages(
	const gchar	 *source_dir,
	GHashTable	 *by_slug,
	GError		**error
){
	g_autofree gchar *docs_dir = g_build_filename(source_dir, "docs", NULL);
	g_autofree gchar *index_path = g_build_filename(docs_dir, "index.org", NULL);
	g_autofree gchar *readme_path = g_build_filename(source_dir, "README.org", NULL);
	g_autoptr(GPtrArray) pages = g_ptr_array_new_with_free_func((GDestroyNotify)page_free);
	g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GDir) dir = NULL;
	const gchar *name;
	guint i;

	if (!g_file_test(index_path, G_FILE_TEST_IS_REGULAR))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "%s has no docs/index.org; the nav comes from it, "
		            "so there is no site without one", source_dir);
		return NULL;
	}

	dir = g_dir_open(docs_dir, 0, error);

	if (NULL == dir)
		return NULL;

	while (NULL != (name = g_dir_read_name(dir)))
	{
		if (g_str_has_suffix(name, ".org"))
			g_ptr_array_add(names, g_strdup(name));
	}

	g_ptr_array_sort(names, compare_names);

	for (i = 0; i < names->len; i++)
	{
		g_autofree gchar *relative = g_build_filename("docs",
			g_ptr_array_index(names, i), NULL);
		Page *page = page_load(source_dir, relative, error);

		if (NULL == page)
			return NULL;

		g_ptr_array_add(pages, page);
	}

	if (g_file_test(readme_path, G_FILE_TEST_IS_REGULAR))
	{
		Page *page = page_load(source_dir, "README.org", error);

		if (NULL == page)
			return NULL;

		g_ptr_array_add(pages, page);
	}

	for (i = 0; i < pages->len; i++)
	{
		Page *page = g_ptr_array_index(pages, i);
		Page *other = g_hash_table_lookup(by_slug, page->slug);

		if (NULL != other)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			            "%s and %s would both become %s.html",
			            other->relative, page->relative, page->slug);
			return NULL;
		}

		g_hash_table_insert(by_slug, page->slug, page);
	}

	return g_steal_pointer(&pages);
}

/* ------------------------------------------------------------------------
 * 2. Resolve
 * ---------------------------------------------------------------------- */

typedef struct
{
	VentureDocsSite	*self;
	Page		*page;
	GHashTable	*by_slug;
	GHashTable	*linked_from_index;	/* slug -> TRUE, filled for index only */
	GError		*error;
} ResolveContext;

/*
 * The page a file link from @page names, or NULL with @error set. The
 * target is taken relative to the page's own directory, as Org does,
 * and must land inside the source tree: "../../../etc/passwd" is a
 * broken link, not a way out.
 */
static Page *
resolve_target(
	ResolveContext	 *ctx,
	const gchar	 *target,
	gboolean	 *exists_as_file,
	gchar		**basename_out
){
	g_autofree gchar *dir = g_path_get_dirname(ctx->page->path);
	g_autofree gchar *joined = g_build_filename(dir, target, NULL);
	g_autofree gchar *canonical = g_canonicalize_filename(joined, NULL);
	g_autofree gchar *root = g_canonicalize_filename(ctx->self->source_dir, NULL);
	g_autofree gchar *bounded = g_strconcat(root, G_DIR_SEPARATOR_S, NULL);
	g_autofree gchar *slug = NULL;
	Page *found;

	*exists_as_file = FALSE;
	*basename_out = g_path_get_basename(target);

	if (!g_str_has_prefix(canonical, bounded))
		return NULL;

	if (!g_file_test(canonical, G_FILE_TEST_IS_REGULAR))
		return NULL;

	*exists_as_file = TRUE;

	if (!g_str_has_suffix(canonical, ".org"))
		return NULL;

	slug = slug_for(canonical);
	found = g_hash_table_lookup(ctx->by_slug, slug);

	if (NULL == found)
		return NULL;

	/* Same slug, but is it the same file? docs/x.org and a stray
	 * x.org elsewhere in the tree are different documents. */
	if (0 != g_strcmp0(found->path, canonical))
	{
		g_autofree gchar *found_canonical = g_canonicalize_filename(found->path, NULL);

		if (0 != g_strcmp0(found_canonical, canonical))
			return NULL;
	}

	return found;
}

/*
 * What a "::" search on a file link becomes: "*Heading" is that
 * heading's anchor, "#custom" a custom id, anything else nothing.
 */
static gchar *
anchor_for_search(const gchar *search)
{
	if (NULL == search || '\0' == *search)
		return NULL;

	if ('*' == *search)
		return venture_org_heading_slug(search + 1);

	if ('#' == *search)
		return g_strdup(search + 1);

	return NULL;
}

static gboolean
rewrite_link(
	const GMatchInfo	*match,
	GString			*result,
	gpointer		 user_data
){
	ResolveContext *ctx = user_data;
	g_autofree gchar *target = g_match_info_fetch(match, 1);
	g_autofree gchar *description = g_match_info_fetch(match, 3);
	g_autofree gchar *file_part = NULL;
	g_autofree gchar *anchor = NULL;
	g_autofree gchar *basename = NULL;
	const gchar *search;
	gboolean exists;
	Page *page;

	if (NULL != ctx->error)
		return TRUE;

	search = strstr(target, "::");
	file_part = (NULL != search) ? g_strndup(target, search - target) : g_strdup(target);
	anchor = anchor_for_search((NULL != search) ? search + 2 : NULL);

	page = resolve_target(ctx, file_part, &exists, &basename);

	if (NULL != page)
	{
		if (NULL != ctx->linked_from_index)
			g_hash_table_add(ctx->linked_from_index, g_strdup(page->slug));

		g_string_append_printf(result, "[[file:%s.html", page->slug);

		if (NULL != anchor)
			g_string_append_printf(result, "#%s", anchor);

		g_string_append(result, "][");

		if (NULL != description && '\0' != *description)
			g_string_append(result, description);
		else
			g_string_append(result, page->title);

		g_string_append(result, "]]");
		return FALSE;
	}

	if (exists)
	{
		/*
		 * A real file that is not a document -- the Containerfile, a
		 * plugin's source -- is named rather than linked: the site is
		 * flat and read-only, and a link into nowhere is the thing
		 * this pass exists to prevent.
		 */
		g_string_append_c(result, '=');
		g_string_append(result, (NULL != description && '\0' != *description)
			? description : basename);
		g_string_append_c(result, '=');
		return FALSE;
	}

	g_set_error(&ctx->error, VENTURE_ERROR, VENTURE_ERROR_DOCS_BROKEN_LINK,
	            "%s links to %s, which does not exist",
	            ctx->page->relative, file_part);

	return TRUE;
}

/*
 * Rewrites every file link in @page. For the index, also records which
 * pages it links -- the set the unlinked check reads.
 */
static gboolean
resolve_page(
	VentureDocsSite	 *self,
	Page		 *page,
	GHashTable	 *by_slug,
	GHashTable	 *linked_from_index,
	GError		**error
){
	static GRegex *link_regex = NULL;
	ResolveContext ctx;
	g_autofree gchar *rewritten = NULL;

	if (NULL == link_regex)
	{
		link_regex = g_regex_new(
			"\\[\\[file:([^\\]\\[]+)\\](\\[([^\\]]*)\\])?\\]",
			G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, NULL);
		g_assert(NULL != link_regex);
	}

	ctx.self = self;
	ctx.page = page;
	ctx.by_slug = by_slug;
	ctx.linked_from_index = linked_from_index;
	ctx.error = NULL;

	rewritten = g_regex_replace_eval(link_regex, page->org, -1, 0, 0,
	                                 rewrite_link, &ctx, error);

	if (NULL != ctx.error)
	{
		g_clear_error(error);
		g_propagate_error(error, ctx.error);
		return FALSE;
	}

	if (NULL == rewritten)
		return FALSE;

	g_free(page->org);
	page->org = g_steal_pointer(&rewritten);

	return TRUE;
}

/* ------------------------------------------------------------------------
 * The nav, from docs/index.org
 * ---------------------------------------------------------------------- */

/*
 * A heading opens a group; a list item whose text is a link to a page
 * joins the current group. Groups with no entries are not shown, so
 * the index's prose sections cost nothing.
 */
static GPtrArray *
parse_nav(
	const gchar	*index_org,
	GHashTable	*by_slug
){
	static GRegex *item_regex = NULL;
	GPtrArray *groups = g_ptr_array_new_with_free_func((GDestroyNotify)nav_group_free);
	g_auto(GStrv) lines = g_strsplit(index_org, "\n", -1);
	NavGroup *current = NULL;
	guint i;

	if (NULL == item_regex)
	{
		item_regex = g_regex_new(
			"^\\s*[-+]\\s+\\[\\[file:([a-z0-9_-]+)\\.html[^\\]]*\\]\\[([^\\]]*)\\]\\]",
			G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, NULL);
		g_assert(NULL != item_regex);
	}

	for (i = 0; NULL != lines[i]; i++)
	{
		const gchar *line = lines[i];
		g_autoptr(GMatchInfo) match = NULL;

		if ('*' == line[0])
		{
			const gchar *text = line + strspn(line, "*");

			if (' ' == *text)
			{
				g_autofree gchar *title = g_strdup(text + 1);

				current = g_new0(NavGroup, 1);
				current->title = g_strdup(g_strstrip(title));
				current->entries = g_ptr_array_new_with_free_func(
					(GDestroyNotify)nav_entry_free);
				g_ptr_array_add(groups, current);
				continue;
			}
		}

		if (NULL == current)
			continue;

		if (g_regex_match(item_regex, line, 0, &match))
		{
			g_autofree gchar *slug = g_match_info_fetch(match, 1);
			g_autofree gchar *title = g_match_info_fetch(match, 2);
			NavEntry *entry;

			if (NULL == g_hash_table_lookup(by_slug, slug))
				continue;

			entry = g_new0(NavEntry, 1);
			entry->slug = g_steal_pointer(&slug);
			entry->title = g_steal_pointer(&title);
			g_ptr_array_add(current->entries, entry);
		}
	}

	/* Drop the empty groups. */
	for (i = groups->len; i > 0; i--)
	{
		NavGroup *group = g_ptr_array_index(groups, i - 1);

		if (0 == group->entries->len)
			g_ptr_array_remove_index(groups, i - 1);
	}

	return groups;
}

/* ------------------------------------------------------------------------
 * 3. Render
 * ---------------------------------------------------------------------- */

/*
 * Emacs gives every heading a random id. The site needs the same
 * anchor whichever renderer built the page, so each heading's id is
 * replaced by the slug of its text -- the slug the link resolver
 * already used for "::*Heading" searches.
 */
static gboolean
replace_heading_id(
	const GMatchInfo	*match,
	GString			*result,
	gpointer		 user_data
){
	g_autofree gchar *level = g_match_info_fetch(match, 1);
	g_autofree gchar *inner = g_match_info_fetch(match, 2);
	g_autofree gchar *text = venture_org_html_to_text(inner);
	g_autofree gchar *slug = venture_org_heading_slug(text);

	g_string_append_printf(result, "<h%s id=\"", level);
	venture_org_html_escape_append(result, slug);
	g_string_append_printf(result, "\">%s</h%s>", inner, level);

	return FALSE;
}

static gchar *
stabilise_heading_ids(
	const gchar	 *html,
	GError		**error
){
	static GRegex *heading_regex = NULL;

	if (NULL == heading_regex)
	{
		heading_regex = g_regex_new(
			"<h([2-6]) id=\"[^\"]*\">(.*?)</h\\1>",
			G_REGEX_DOTALL, G_REGEX_MATCH_DEFAULT, NULL);
		g_assert(NULL != heading_regex);
	}

	return g_regex_replace_eval(heading_regex, html, -1, 0, 0,
	                            replace_heading_id, NULL, error);
}

static const gchar venture_docs_emacs_script[] =
	"(require 'org)\n"
	"(require 'ox-html)\n"
	"(setq org-export-with-toc nil\n"
	"      org-export-with-section-numbers nil\n"
	"      org-export-with-sub-superscripts nil\n"
	"      org-export-with-smart-quotes nil\n"
	"      org-export-with-special-strings nil\n"
	"      org-export-with-author nil\n"
	"      org-export-with-date nil\n"
	"      org-export-with-email nil\n"
	"      org-export-with-creator nil\n"
	"      org-export-with-broken-links 'mark\n"
	"      org-export-use-babel nil\n"
	"      org-html-htmlize-output-type nil\n"
	"      org-html-link-org-files-as-html t)\n"
	"(dolist (file (directory-files (getenv \"VENTURE_DOCS_WORK\") t \"\\\\.org\\\\'\"))\n"
	"  (with-current-buffer (find-file-noselect file)\n"
	"    (org-export-to-file 'html\n"
	"      (concat (file-name-sans-extension file) \".body.html\")\n"
	"      nil nil nil t)))\n";

/*
 * One Emacs process for every page: it is the start-up that costs,
 * not the export. The rewritten org files go to a private directory,
 * Emacs writes a body beside each, and the bodies are read back with
 * their heading ids made stable.
 */
static gboolean
render_with_emacs(
	GPtrArray	 *pages,
	const gchar	 *emacs,
	GCancellable	 *cancellable,
	GError		**error
){
	g_autofree gchar *work = g_dir_make_tmp("venture-docs-emacs-XXXXXX", error);
	g_autofree gchar *script = NULL;
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) process = NULL;
	g_autofree gchar *stderr_text = NULL;
	gboolean ok = TRUE;
	guint i;

	if (NULL == work)
		return FALSE;

	script = g_build_filename(work, "render.el", NULL);

	if (!g_file_set_contents(script, venture_docs_emacs_script, -1, error))
		ok = FALSE;

	for (i = 0; ok && i < pages->len; i++)
	{
		Page *page = g_ptr_array_index(pages, i);
		g_autofree gchar *name = g_strconcat(page->slug, ".org", NULL);
		g_autofree gchar *path = g_build_filename(work, name, NULL);

		if (!g_file_set_contents(path, page->org, -1, error))
			ok = FALSE;
	}

	if (ok)
	{
		launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDERR_PIPE |
		                                     G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
		g_subprocess_launcher_setenv(launcher, "VENTURE_DOCS_WORK", work, TRUE);
		g_subprocess_launcher_setenv(launcher, "TERM", "dumb", TRUE);
		process = g_subprocess_launcher_spawn(launcher, error,
			emacs, "-Q", "--batch", "-l", script, NULL);

		if (NULL == process)
			ok = FALSE;
	}

	if (ok && !g_subprocess_communicate_utf8(process, NULL, cancellable,
	                                          NULL, &stderr_text, error))
		ok = FALSE;

	if (ok && !g_subprocess_get_if_exited(process))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "emacs did not exit normally: %s",
		            (NULL != stderr_text) ? stderr_text : "");
		ok = FALSE;
	}

	if (ok && 0 != g_subprocess_get_exit_status(process))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "emacs exited with status %d:\n%s",
		            g_subprocess_get_exit_status(process),
		            (NULL != stderr_text) ? stderr_text : "");
		ok = FALSE;
	}

	for (i = 0; ok && i < pages->len; i++)
	{
		Page *page = g_ptr_array_index(pages, i);
		g_autofree gchar *name = g_strconcat(page->slug, ".body.html", NULL);
		g_autofree gchar *path = g_build_filename(work, name, NULL);
		g_autofree gchar *raw = NULL;

		if (!g_file_get_contents(path, &raw, NULL, NULL))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
			            "emacs produced no HTML for %s:\n%s", page->relative,
			            (NULL != stderr_text) ? stderr_text : "");
			ok = FALSE;
			break;
		}

		page->body = stabilise_heading_ids(raw, error);

		if (NULL == page->body)
			ok = FALSE;
	}

	/* The work directory is ours alone; nothing in it is worth keeping. */
	{
		g_autoptr(GDir) dir = g_dir_open(work, 0, NULL);
		const gchar *entry;

		while (NULL != dir && NULL != (entry = g_dir_read_name(dir)))
		{
			g_autofree gchar *path = g_build_filename(work, entry, NULL);

			g_unlink(path);
		}

		g_rmdir(work);
	}

	return ok;
}

static gboolean
render_with_builtin(GPtrArray *pages)
{
	guint i;

	for (i = 0; i < pages->len; i++)
	{
		Page *page = g_ptr_array_index(pages, i);

		page->body = venture_org_to_html(page->org);
	}

	return TRUE;
}

/* ------------------------------------------------------------------------
 * 4. Write
 * ---------------------------------------------------------------------- */

static void
append_nav(
	GString		*html,
	GPtrArray	*groups,
	const gchar	*current_slug
){
	guint g;

	g_string_append(html,
		"<nav class=\"docs-nav\" aria-label=\"Documentation\">\n"
		"<a class=\"docs-brand\" href=\"index.html\">VENTURE docs</a>\n"
		"<div class=\"docs-search-box\">"
		"<input id=\"docs-search\" type=\"search\" placeholder=\"Search the docs\" "
		"aria-label=\"Search the documentation\" autocomplete=\"off\">"
		"<ul id=\"docs-results\" class=\"docs-results\" hidden></ul></div>\n");

	for (g = 0; g < groups->len; g++)
	{
		NavGroup *group = g_ptr_array_index(groups, g);
		guint e;

		g_string_append(html, "<section class=\"docs-nav-group\">\n<h2>");
		venture_org_html_escape_append(html, group->title);
		g_string_append(html, "</h2>\n<ul>\n");

		for (e = 0; e < group->entries->len; e++)
		{
			NavEntry *entry = g_ptr_array_index(group->entries, e);
			gboolean current = (0 == g_strcmp0(entry->slug, current_slug));

			g_string_append(html, "<li><a href=\"");
			venture_org_html_escape_append(html, entry->slug);
			g_string_append(html, ".html\"");

			if (current)
				g_string_append(html, " aria-current=\"page\"");

			g_string_append_c(html, '>');
			venture_org_html_escape_append(html, entry->title);
			g_string_append(html, "</a></li>\n");
		}

		g_string_append(html, "</ul>\n</section>\n");
	}

	g_string_append(html, "</nav>\n");
}

static gchar *
render_page(
	Page		*page,
	GPtrArray	*groups
){
	GString *html = g_string_new(
		"<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
		"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
		"<title>");

	venture_org_html_escape_append(html, page->title);
	g_string_append(html, " &mdash; VENTURE documentation</title>\n");

	if (NULL != page->description)
	{
		g_string_append(html, "<meta name=\"description\" content=\"");
		venture_org_html_escape_append(html, page->description);
		g_string_append(html, "\">\n");
	}

	g_string_append(html,
		"<link rel=\"stylesheet\" href=\"venture-docs.css\">\n"
		"</head>\n<body>\n<div class=\"docs\">\n");
	append_nav(html, groups, page->slug);
	g_string_append(html, "<main class=\"docs-main\">\n<article class=\"docs-article\">\n<h1>");
	venture_org_html_escape_append(html, page->title);
	g_string_append(html, "</h1>\n");

	if (NULL != page->description)
	{
		g_string_append(html, "<p class=\"docs-lede\">");
		venture_org_html_escape_append(html, page->description);
		g_string_append(html, "</p>\n");
	}

	g_string_append(html, page->body);
	g_string_append(html,
		"</article>\n</main>\n</div>\n"
		"<script src=\"venture-docs.js\"></script>\n</body>\n</html>\n");

	return g_string_free(html, FALSE);
}

static gchar *
render_search_index(GPtrArray *pages)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) root = NULL;
	guint i;

	json_builder_begin_array(builder);

	for (i = 0; i < pages->len; i++)
	{
		Page *page = g_ptr_array_index(pages, i);
		g_autofree gchar *url = g_strconcat(page->slug, ".html", NULL);
		g_autofree gchar *text = venture_org_html_to_text(page->body);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "url");
		json_builder_add_string_value(builder, url);
		json_builder_set_member_name(builder, "title");
		json_builder_add_string_value(builder, page->title);
		json_builder_set_member_name(builder, "description");
		json_builder_add_string_value(builder,
			(NULL != page->description) ? page->description : "");
		json_builder_set_member_name(builder, "text");
		json_builder_add_string_value(builder, text);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	root = json_builder_get_root(builder);

	return venture_json_to_string(root, FALSE);
}

/*
 * Whatever an earlier build left at the top of the output directory is
 * removed first, so a document that was deleted does not linger as a
 * page the server still serves. Only this generator's own kinds of
 * file are touched; the directory may be shared with something else.
 */
static void
clear_previous_output(const gchar *output_dir)
{
	g_autoptr(GDir) dir = g_dir_open(output_dir, 0, NULL);
	const gchar *entry;

	while (NULL != dir && NULL != (entry = g_dir_read_name(dir)))
	{
		if (g_str_has_suffix(entry, ".html") ||
		    0 == g_strcmp0(entry, "venture-docs.css") ||
		    0 == g_strcmp0(entry, "venture-docs.js") ||
		    0 == g_strcmp0(entry, "search-index.json"))
		{
			g_autofree gchar *path = g_build_filename(output_dir, entry, NULL);

			g_unlink(path);
		}
	}
}

static gboolean
write_output(
	const gchar	 *output_dir,
	const gchar	 *name,
	const gchar	 *content,
	GError		**error
){
	g_autofree gchar *path = g_build_filename(output_dir, name, NULL);

	return g_file_set_contents(path, content, -1, error);
}

static gboolean
write_site(
	VentureDocsSite	 *self,
	GPtrArray	 *pages,
	GPtrArray	 *groups,
	GError		**error
){
	g_autofree gchar *search = NULL;
	guint i;

	if (0 != g_mkdir_with_parents(self->output_dir, 0755))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		            "cannot create %s: %s", self->output_dir, g_strerror(errno));
		return FALSE;
	}

	clear_previous_output(self->output_dir);

	for (i = 0; i < pages->len; i++)
	{
		Page *page = g_ptr_array_index(pages, i);
		g_autofree gchar *name = g_strconcat(page->slug, ".html", NULL);
		g_autofree gchar *html = render_page(page, groups);

		if (!write_output(self->output_dir, name, html, error))
			return FALSE;
	}

	search = render_search_index(pages);

	return write_output(self->output_dir, "search-index.json", search, error) &&
	       write_output(self->output_dir, "venture-docs.css",
	                    venture_docs_asset_venture_docs_css, error) &&
	       write_output(self->output_dir, "venture-docs.js",
	                    venture_docs_asset_venture_docs_js, error);
}

/* ------------------------------------------------------------------------
 * The build
 * ---------------------------------------------------------------------- */

/**
 * venture_docs_site_build:
 * @self: the site
 * @cancellable: (nullable): to stop a long render
 * @error: return location for an error
 *
 * Renders the site. Nothing is written unless every link resolves and
 * every document is reachable from docs/index.org; a refusal names the
 * document and the link, as %VENTURE_ERROR_DOCS_BROKEN_LINK or
 * %VENTURE_ERROR_DOCS_UNLINKED. A missing docs/index.org is
 * %VENTURE_ERROR_NOT_FOUND. Asking for Emacs on a machine without one
 * is %VENTURE_ERROR_UNSUPPORTED; the automatic choice falls back to the
 * builtin renderer instead.
 *
 * Returns: %TRUE if the site was written
 */
gboolean
venture_docs_site_build(
	VentureDocsSite	 *self,
	GCancellable	 *cancellable,
	GError		**error
){
	g_autoptr(GHashTable) by_slug = NULL;
	g_autoptr(GHashTable) linked = NULL;
	g_autoptr(GPtrArray) pages = NULL;
	g_autoptr(GPtrArray) groups = NULL;
	g_autofree gchar *emacs = NULL;
	Page *index = NULL;
	guint i;

	g_return_val_if_fail(VENTURE_IS_DOCS_SITE(self), FALSE);
	g_return_val_if_fail(NULL == error || NULL == *error, FALSE);

	self->page_count = 0;

	/* Decide the renderer first: an impossible request is refused
	 * before any work, not after the checks. */
	self->renderer_used = self->renderer;

	if (VENTURE_DOCS_RENDERER_BUILTIN != self->renderer)
	{
		emacs = g_find_program_in_path("emacs");

		if (NULL == emacs)
		{
			if (VENTURE_DOCS_RENDERER_EMACS == self->renderer)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED,
				                    "emacs is not on PATH; use the builtin renderer");
				return FALSE;
			}

			self->renderer_used = VENTURE_DOCS_RENDERER_BUILTIN;
		}
		else
			self->renderer_used = VENTURE_DOCS_RENDERER_EMACS;
	}

	by_slug = g_hash_table_new(g_str_hash, g_str_equal);
	pages = collect_pages(self->source_dir, by_slug, error);

	if (NULL == pages)
		return FALSE;

	index = g_hash_table_lookup(by_slug, "index");
	linked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < pages->len; i++)
	{
		Page *page = g_ptr_array_index(pages, i);

		if (!resolve_page(self, page, by_slug,
		                  (page == index) ? linked : NULL, error))
			return FALSE;
	}

	for (i = 0; i < pages->len; i++)
	{
		Page *page = g_ptr_array_index(pages, i);

		if (page == index || g_hash_table_contains(linked, page->slug))
			continue;

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_DOCS_UNLINKED,
		            "%s is not linked from docs/index.org, so nobody "
		            "could find it in the nav", page->relative);
		return FALSE;
	}

	if (g_cancellable_set_error_if_cancelled(cancellable, error))
		return FALSE;

	groups = parse_nav(index->org, by_slug);

	if (VENTURE_DOCS_RENDERER_EMACS == self->renderer_used)
	{
		if (!render_with_emacs(pages, emacs, cancellable, error))
			return FALSE;
	}
	else
		render_with_builtin(pages);

	if (!write_site(self, pages, groups, error))
		return FALSE;

	self->page_count = pages->len;

	return TRUE;
}

/* ------------------------------------------------------------------------
 * GObject
 * ---------------------------------------------------------------------- */

static void
venture_docs_site_get_property(
	GObject		*object,
	guint		 property_id,
	GValue		*value,
	GParamSpec	*pspec
){
	VentureDocsSite *self = VENTURE_DOCS_SITE(object);

	switch (property_id)
	{
	case PROP_SOURCE_DIR:
		g_value_set_string(value, self->source_dir);
		break;

	case PROP_OUTPUT_DIR:
		g_value_set_string(value, self->output_dir);
		break;

	case PROP_RENDERER:
		g_value_set_enum(value, self->renderer);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
venture_docs_site_set_property(
	GObject		*object,
	guint		 property_id,
	const GValue	*value,
	GParamSpec	*pspec
){
	VentureDocsSite *self = VENTURE_DOCS_SITE(object);

	switch (property_id)
	{
	case PROP_SOURCE_DIR:
		g_free(self->source_dir);
		self->source_dir = g_value_dup_string(value);
		break;

	case PROP_OUTPUT_DIR:
		g_free(self->output_dir);
		self->output_dir = g_value_dup_string(value);
		break;

	case PROP_RENDERER:
		self->renderer = g_value_get_enum(value);
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
	}
}

static void
venture_docs_site_finalize(GObject *object)
{
	VentureDocsSite *self = VENTURE_DOCS_SITE(object);

	g_clear_pointer(&self->source_dir, g_free);
	g_clear_pointer(&self->output_dir, g_free);

	G_OBJECT_CLASS(venture_docs_site_parent_class)->finalize(object);
}

static void
venture_docs_site_class_init(VentureDocsSiteClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	object_class->get_property = venture_docs_site_get_property;
	object_class->set_property = venture_docs_site_set_property;
	object_class->finalize = venture_docs_site_finalize;

	/**
	 * VentureDocsSite:source-dir:
	 *
	 * The repository root: the directory holding docs/ and README.org.
	 */
	properties[PROP_SOURCE_DIR] = g_param_spec_string(
		"source-dir", "Source directory",
		"The directory holding docs/ and README.org",
		NULL,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

	/**
	 * VentureDocsSite:output-dir:
	 *
	 * Where the site is written. Created if it does not exist.
	 */
	properties[PROP_OUTPUT_DIR] = g_param_spec_string(
		"output-dir", "Output directory",
		"Where the rendered site is written",
		NULL,
		G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

	/**
	 * VentureDocsSite:renderer:
	 *
	 * Which renderer turns org into HTML bodies. The default, auto,
	 * uses Emacs when it is on PATH and the builtin otherwise.
	 */
	properties[PROP_RENDERER] = g_param_spec_enum(
		"renderer", "Renderer",
		"Emacs, the builtin renderer, or whichever is available",
		VENTURE_TYPE_DOCS_RENDERER, VENTURE_DOCS_RENDERER_AUTO,
		G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
venture_docs_site_init(VentureDocsSite *self)
{
	self->renderer = VENTURE_DOCS_RENDERER_AUTO;
	self->renderer_used = VENTURE_DOCS_RENDERER_AUTO;
}

/**
 * venture_docs_site_new:
 * @source_dir: the repository root, holding docs/ and README.org
 * @output_dir: where to write the site
 *
 * Returns: (transfer full): a site ready to build
 */
VentureDocsSite *
venture_docs_site_new(
	const gchar	*source_dir,
	const gchar	*output_dir
){
	g_return_val_if_fail(NULL != source_dir, NULL);
	g_return_val_if_fail(NULL != output_dir, NULL);

	return g_object_new(VENTURE_TYPE_DOCS_SITE,
	                    "source-dir", source_dir,
	                    "output-dir", output_dir,
	                    NULL);
}

/**
 * venture_docs_site_get_source_dir:
 * @self: the site
 *
 * Returns: the repository root the site is rendered from
 */
const gchar *
venture_docs_site_get_source_dir(VentureDocsSite *self)
{
	g_return_val_if_fail(VENTURE_IS_DOCS_SITE(self), NULL);

	return self->source_dir;
}

/**
 * venture_docs_site_get_output_dir:
 * @self: the site
 *
 * Returns: where the site is written
 */
const gchar *
venture_docs_site_get_output_dir(VentureDocsSite *self)
{
	g_return_val_if_fail(VENTURE_IS_DOCS_SITE(self), NULL);

	return self->output_dir;
}

/**
 * venture_docs_site_get_renderer:
 * @self: the site
 *
 * Returns: the renderer asked for
 */
VentureDocsRenderer
venture_docs_site_get_renderer(VentureDocsSite *self)
{
	g_return_val_if_fail(VENTURE_IS_DOCS_SITE(self), VENTURE_DOCS_RENDERER_AUTO);

	return self->renderer;
}

/**
 * venture_docs_site_set_renderer:
 * @self: the site
 * @renderer: the renderer to use
 *
 * Chooses the renderer for the next build.
 */
void
venture_docs_site_set_renderer(
	VentureDocsSite		*self,
	VentureDocsRenderer	 renderer
){
	g_return_if_fail(VENTURE_IS_DOCS_SITE(self));

	if (renderer == self->renderer)
		return;

	self->renderer = renderer;
	g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RENDERER]);
}

/**
 * venture_docs_site_get_renderer_used:
 * @self: the site
 *
 * Which renderer the last build actually used: for an automatic
 * choice, whichever was available.
 *
 * Returns: the renderer used, or auto if nothing has been built
 */
VentureDocsRenderer
venture_docs_site_get_renderer_used(VentureDocsSite *self)
{
	g_return_val_if_fail(VENTURE_IS_DOCS_SITE(self), VENTURE_DOCS_RENDERER_AUTO);

	return self->renderer_used;
}

/**
 * venture_docs_site_get_page_count:
 * @self: the site
 *
 * Returns: how many pages the last successful build wrote
 */
guint
venture_docs_site_get_page_count(VentureDocsSite *self)
{
	g_return_val_if_fail(VENTURE_IS_DOCS_SITE(self), 0);

	return self->page_count;
}
