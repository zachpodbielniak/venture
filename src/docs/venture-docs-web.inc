/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-docs-web.inc - /docs: the rendered documentation, served read-only
 *
 * Included by venture-web-server.c. The site itself is what `make
 * docs-site` (or `venturectl docs build`) wrote to the directory the
 * docs.site_dir setting names; the server never renders anything. No
 * session is needed: the documentation is public by design, so an
 * install documents itself to anyone who can reach its sign-in page.
 *
 * Only the top level of that directory is reachable, and only by the
 * kinds of file the generator writes -- a page, the stylesheet, the
 * script, the search index. There are no subdirectories to reach, so
 * the page name is checked against a pattern rather than resolved.
 */

static const gchar *
venture_docs_web_content_type(const gchar *name)
{
	if (g_str_has_suffix(name, ".html"))
		return "text/html; charset=utf-8";

	if (g_str_has_suffix(name, ".css"))
		return "text/css; charset=utf-8";

	if (g_str_has_suffix(name, ".js"))
		return "application/javascript; charset=utf-8";

	return "application/json; charset=utf-8";
}

/*
 * An instance whose site was never rendered answers with what to run,
 * as a page, rather than a bare 404 or the server's JSON error: the
 * person reading it is the one who can fix it.
 */
static HtmxResponse *
venture_docs_web_not_built(const gchar *directory)
{
	GString *body = g_string_new(
		"<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\">"
		"<title>Documentation not built</title></head><body>"
		"<h1>The documentation has not been rendered</h1>"
		"<p>This server looks for the site in <code>");

	venture_org_html_escape_append(body, directory);
	g_string_append(body,
		"</code> (the <code>docs.site_dir</code> setting). Render it with "
		"<code>make docs-site</code> in a checkout, or "
		"<code>venturectl docs build source=CHECKOUT output=");
	venture_org_html_escape_append(body, directory);
	g_string_append(body, "</code>.</p></body></html>\n");

	return venture_web_html_response(g_string_free(body, FALSE), 404);
}

static HtmxResponse *
venture_docs_web_index(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;

	(void)request;
	(void)params;

	if (!venture_web_module_enabled(self, "docs"))
		return htmx_response_not_found();

	return venture_web_redirect_to("/docs/index.html");
}

static HtmxResponse *
venture_docs_web_page(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	static GRegex *name_regex = NULL;
	VentureWebServer *self = user_data;
	const gchar *name = g_hash_table_lookup(params, "page");
	g_autofree gchar *directory = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *content = NULL;
	gsize length = 0;
	HtmxResponse *response;

	(void)request;

	if (!venture_web_module_enabled(self, "docs"))
		return htmx_response_not_found();

	if (NULL == name_regex)
	{
		name_regex = g_regex_new("^[A-Za-z0-9][A-Za-z0-9_-]*\\.(html|css|js|json)$",
		                         G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, NULL);
		g_assert(NULL != name_regex);
	}

	if (NULL == name || !g_regex_match(name_regex, name, 0, NULL))
		return htmx_response_not_found();

	g_object_get(venture_context_get_config(self->context),
	             "docs-site-dir", &directory, NULL);

	if (venture_string_is_empty(directory))
		return htmx_response_not_found();

	path = g_build_filename(directory, name, NULL);

	if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
	{
		g_autofree gchar *index = g_build_filename(directory, "index.html", NULL);

		/* No site at all is a different answer from no such page. */
		if (!g_file_test(index, G_FILE_TEST_IS_REGULAR) && g_str_has_suffix(name, ".html"))
			return venture_docs_web_not_built(directory);

		return htmx_response_not_found();
	}

	if (!g_file_get_contents(path, &content, &length, NULL))
		return htmx_response_not_found();

	response = htmx_response_new_with_content(content);
	htmx_response_set_content_type(response, venture_docs_web_content_type(name));
	htmx_response_set_status(response, 200);
	htmx_response_add_header(response, "Cache-Control", "public, max-age=300");

	return response;
}

static void
venture_docs_web_register(
	HtmxRouter		*router,
	VentureWebServer	*self
){
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/docs", VENTURE_DATA_CLASS_REFERENCE, VENTURE_HOSTED_ROUTE_NONE, venture_docs_web_index, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/docs/", VENTURE_DATA_CLASS_REFERENCE, VENTURE_HOSTED_ROUTE_NONE, venture_docs_web_index, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/docs/:page", VENTURE_DATA_CLASS_REFERENCE, VENTURE_HOSTED_ROUTE_NONE, venture_docs_web_page, self);
}
