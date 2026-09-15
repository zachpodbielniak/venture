/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_commerce(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) response = NULL, body = NULL;
	if (args[1] == NULL || g_strcmp0(args[1], "import"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl commerce import [JSON]");
		return -1;
	}
	if (args[2] != NULL)
	{
		if (!json_parser_load_from_data(parser, args[2], -1, error)) return -1;
		body = json_node_copy(json_parser_get_root(parser));
	}
	response = venture_cli_request(cli, "POST", "/api/v1/commerce/import", body, error);
	if (response == NULL) return -1;
	venture_cli_output(cli, response);
	return 0;
}
