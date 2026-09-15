/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_tax_filing(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *path = NULL;
	const gchar *method = "POST";
	if (args[1] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl tax-filing prepare|review|submit|acknowledge|amend [ID] [field=value ...]");
		return -1;
	}
	if (g_strcmp0(args[1], "prepare") == 0)
	{
		path = g_strdup("/api/v1/tax-filings/prepare");
		body = venture_cli_values_from_args(args, 2);
	}
	else if (args[2] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl tax-filing prepare|review|submit|acknowledge|amend [ID] [field=value ...]");
		return -1;
	}
	else
	{
		g_autofree gchar *id = g_uri_escape_string(args[2], NULL, FALSE);
		path = g_strdup_printf("/api/v1/tax-filings/%s/%s", id, args[1]);
		body = venture_cli_values_from_args(args, 3);
	}
	response = venture_cli_request(cli, method, path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}

static gint
venture_cli_command_contractor_tax(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *path = NULL;
	const gchar *method = "POST";
	if (args[1] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl contractor-tax prepare|review|approve|export [ID] [field=value ...]");
		return -1;
	}
	if (g_strcmp0(args[1], "prepare") == 0)
	{
		path = g_strdup("/api/v1/contractor-tax/prepare");
		body = venture_cli_values_from_args(args, 2);
	}
	else if (args[2] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl contractor-tax prepare|review|approve|export [ID] [field=value ...]");
		return -1;
	}
	else if (g_strcmp0(args[1], "export") == 0)
	{
		g_autofree gchar *id = g_uri_escape_string(args[2], NULL, FALSE);
		g_autoptr(GBytes) bytes = NULL;
		path = g_strdup_printf("/api/v1/contractor-tax/%s/export", id);
		bytes = venture_cli_request_bytes(cli, "GET", path, error);
		if (bytes == NULL)
			return -1;
		fwrite(g_bytes_get_data(bytes, NULL), 1, g_bytes_get_size(bytes), stdout);
		return 0;
	}
	else
	{
		g_autofree gchar *id = g_uri_escape_string(args[2], NULL, FALSE);
		path = g_strdup_printf("/api/v1/contractor-tax/%s/%s", id, args[1]);
		body = venture_cli_values_from_args(args, 3);
	}
	response = venture_cli_request(cli, method, path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
