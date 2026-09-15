/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_payroll(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *path = NULL;
	if (args[1] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl payroll import|disburse|reverse ...");
		return -1;
	}
	if (g_strcmp0(args[1], "import") == 0)
	{
		path = g_strdup("/api/v1/payroll/import");
		body = venture_cli_values_from_args(args, 2);
	}
	else if (args[2] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl payroll disburse|reverse ID");
		return -1;
	}
	else
	{
		g_autofree gchar *id = g_uri_escape_string(args[2], NULL, FALSE);
		path = g_strdup_printf("/api/v1/payroll_run/%s/%s", id, args[1]);
		body = venture_cli_values_from_args(args, 3);
	}
	response = venture_cli_request(cli, "POST", path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
