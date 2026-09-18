/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-docs-cli.inc - venturectl docs build
 *
 * Included by cli/main.c. The one venturectl command that talks to no
 * server: it renders the documentation site from a checkout, through
 * the same VentureDocsSite that `make docs-site` uses, so an installed
 * CLI can rebuild the site a hosted instance serves at /docs.
 */

static gint
venture_cli_command_docs(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(VentureDocsSite) site = NULL;
	g_autofree gchar *source = NULL;
	g_autofree gchar *output = NULL;
	VentureDocsRenderer renderer = VENTURE_DOCS_RENDERER_AUTO;
	guint i;

	(void)cli;

	if ((NULL == args[1]) || (0 != g_strcmp0(args[1], "build")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "usage: venturectl docs build [source=DIR] "
		                    "[output=DIR] [renderer=auto|emacs|builtin]");
		return -1;
	}

	for (i = 2; NULL != args[i]; i++)
	{
		const gchar *equals = strchr(args[i], '=');
		g_autofree gchar *key = NULL;

		if (NULL == equals)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not key=value; usage: venturectl docs build "
			            "[source=DIR] [output=DIR] [renderer=auto|emacs|builtin]",
			            args[i]);
			return -1;
		}

		key = g_strndup(args[i], equals - args[i]);

		if (0 == g_strcmp0(key, "source"))
		{
			g_free(source);
			source = g_strdup(equals + 1);
		}
		else if (0 == g_strcmp0(key, "output"))
		{
			g_free(output);
			output = g_strdup(equals + 1);
		}
		else if (0 == g_strcmp0(key, "renderer"))
		{
			if (!venture_docs_renderer_from_nick(equals + 1, &renderer))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				            "\"%s\" is not a renderer; use auto, emacs or builtin",
				            equals + 1);
				return -1;
			}
		}
		else
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "\"%s\" is not an option of docs build; usage: "
			            "venturectl docs build [source=DIR] [output=DIR] "
			            "[renderer=auto|emacs|builtin]", key);
			return -1;
		}
	}

	if (NULL == source)
		source = g_strdup(".");

	if (NULL == output)
		output = g_build_filename(source, "build", "docs-site", NULL);

	site = venture_docs_site_new(source, output);
	venture_docs_site_set_renderer(site, renderer);

	if (!venture_docs_site_build(site, NULL, error))
		return -1;

	g_print("Rendered %u pages to %s with the %s renderer\n",
	        venture_docs_site_get_page_count(site), output,
	        venture_docs_renderer_to_nick(venture_docs_site_get_renderer_used(site)));

	return 0;
}
