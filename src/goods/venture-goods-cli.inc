/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_purchase(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *path = NULL;
	if (args[1] == NULL || args[2] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl purchase approve|send|receive|match|cancel|return ID [field=value ...]");
		return -1;
	}
	id = g_uri_escape_string(args[2], NULL, FALSE);
	path = g_strdup_printf("/api/v1/purchase_order/%s/%s", id, args[1]);
	body = venture_cli_values_from_args(args, 3);
	response = venture_cli_request(cli, "POST", path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}

static gint
venture_cli_command_sales_order(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *path = NULL;
	if (args[1] == NULL || args[2] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl sales-order allocate|ship|invoice|cancel ID [field=value ...]");
		return -1;
	}
	id = g_uri_escape_string(args[2], NULL, FALSE);
	path = g_strdup_printf("/api/v1/sales_order/%s/%s", id, args[1]);
	body = venture_cli_values_from_args(args, 3);
	response = venture_cli_request(cli, "POST", path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
