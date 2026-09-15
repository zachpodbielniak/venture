/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_claim(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *path = NULL;
	if (args[1] == NULL || args[2] == NULL ||
		(g_strcmp0(args[1], "submit") != 0 && g_strcmp0(args[1], "approve") != 0 && g_strcmp0(args[1], "pay") != 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl claim submit|approve|pay ID");
		return -1;
	}
	id = g_uri_escape_string(args[2], NULL, FALSE);
	path = g_strdup_printf("/api/v1/expense_claim/%s/%s", id, args[1]);
	body = venture_cli_values_from_args(args, 3);
	response = venture_cli_request(cli, "POST", path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
