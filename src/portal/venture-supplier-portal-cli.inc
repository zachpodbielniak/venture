/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_supplier(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *path = NULL;
	if (args[1] == NULL || (g_strcmp0(args[1], "invite") != 0 && g_strcmp0(args[1], "revoke") != 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl supplier invite company_id=ID email=ADDR | revoke ID");
		return -1;
	}
	if (g_strcmp0(args[1], "invite") == 0)
	{
		body = venture_cli_values_from_args(args, 2);
		response = venture_cli_request(cli, "POST", "/api/v1/supplier_portal/invite", body, error);
	}
	else
	{
		if (args[2] == NULL)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"usage: venturectl supplier revoke ID");
			return -1;
		}
		path = g_strdup_printf("/api/v1/supplier_portal_access/%s/actions/revoke", args[2]);
		response = venture_cli_request(cli, "POST", path, NULL, error);
	}
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
