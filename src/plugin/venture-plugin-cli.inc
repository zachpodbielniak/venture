/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-plugin-cli.inc - venturectl plugins [list]
 *
 * One thin call on GET /api/v1/plugins: every plugin the server loaded,
 * which runtime loaded it (native, crispy, exec, a venture type) and what
 * its manifest provides (a data source provider, an automation handler).
 * Owner only, because the answer names paths on the server. Included by
 * src/cli/main.c after the market helpers, whose table it uses.
 */

static const VentureCliColumn venture_cli_plugin_columns[] = {
	{ "name", "plugin", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "kind", "kind", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "runtime", "runtime", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "provides", "provides", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "description", "description", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static gint
venture_cli_command_plugins(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;

	if ((NULL != args[1]) && ((0 != g_strcmp0(args[1], "list")) || (NULL != args[2])))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "usage: venturectl plugins [list]\n"
		                    "Example:\n"
		                    "  venturectl plugins list");
		return -1;
	}

	response = venture_cli_request(cli, "GET", "/api/v1/plugins", NULL, error);

	if (NULL == response)
		return -1;

	venture_cli_print_answer(cli, response, NULL, venture_cli_plugin_columns, "No plugins are loaded.");
	return 0;
}
