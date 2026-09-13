/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_bank(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *path = NULL, *contents = NULL;
	const gchar *action, *id, *parameters, *resource;
	if (args[1] == NULL || args[2] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl bank ACTION ID [JSON|@FILE] | bank match AUTO STATEMENT_ID");
		return -1;
	}
	action = args[1]; id = args[2]; parameters = args[3];
	if (!g_strcmp0(action, "match") && !g_strcmp0(id, "AUTO"))
	{
		action = "auto"; id = args[3]; parameters = NULL;
		if (id == NULL)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "AUTO requires a statement ID");
			return -1;
		}
	}
	if (strspn(id, "0123456789") != strlen(id) || *id == '\0')
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Bank action ID must be numeric");
		return -1;
	}
	if (parameters != NULL && *parameters == '@')
	{
		if (!g_file_get_contents(parameters + 1, &contents, NULL, error)) return -1;
		parameters = contents;
	}
	if (!json_parser_load_from_data(parser, parameters != NULL ? parameters : "{}", -1, error)) return -1;
	resource = !strcmp(action, "import") ? "bank_accounts" :
		!strcmp(action, "auto") || !strcmp(action, "reconcile") ? "bank_statements" : "bank_transactions";
	path = g_strdup_printf("/api/v1/%s/%s/%s", resource, id, action);
	response = venture_cli_request(cli, "POST", path, json_parser_get_root(parser), error);
	if (response == NULL) return -1;
	venture_cli_output(cli, response);
	return 0;
}
