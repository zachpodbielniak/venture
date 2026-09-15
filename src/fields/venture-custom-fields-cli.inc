/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_fields(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	const gchar *path;
	if (args[1] == NULL || (g_strcmp0(args[1], "define") != 0 && g_strcmp0(args[1], "layout") != 0 &&
		g_strcmp0(args[1], "value") != 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl fields define|layout|value field=value ...");
		return -1;
	}
	body = venture_cli_values_from_args(args, 2);
	if (g_strcmp0(args[1], "define") == 0)
		path = "/api/v1/accounting_custom_field";
	else if (g_strcmp0(args[1], "layout") == 0)
		path = "/api/v1/accounting_layout";
	else
		path = "/api/v1/custom_field_value";
	response = venture_cli_request(cli, "POST", path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
