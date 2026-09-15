/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_close(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *path = NULL;
	const gchar *method = "POST";
	if (args[1] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl close open|run|sign|complete|reopen|pack ID [field=value ...]");
		return -1;
	}
	if (g_strcmp0(args[1], "open") == 0)
	{
		path = g_strdup("/api/v1/close/open");
		body = venture_cli_values_from_args(args, 2);
	}
	else if (args[2] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl close open|run|sign|complete|reopen|pack ID [field=value ...]");
		return -1;
	}
	else if (g_strcmp0(args[1], "pack") == 0)
	{
		g_autofree gchar *id = g_uri_escape_string(args[2], NULL, FALSE);
		path = g_strdup_printf("/api/v1/close/%s/pack", id);
		method = "GET";
	}
	else
	{
		g_autofree gchar *id = g_uri_escape_string(args[2], NULL, FALSE);
		path = g_strdup_printf("/api/v1/close/%s/%s", id, args[1]);
		body = venture_cli_values_from_args(args, 3);
	}
	response = venture_cli_request(cli, method, path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
