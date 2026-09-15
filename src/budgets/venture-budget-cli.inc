/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_budget(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;
	const gchar *period = args[2] != NULL ? args[2] : "2026-01";
	const gchar *kind = "actual";
	if (args[1] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl budget vs-actual|forecast YYYY-MM");
		return -1;
	}
	if (g_strcmp0(args[1], "forecast") == 0)
		kind = "forecast";
	else if (g_strcmp0(args[1], "vs-actual") != 0 && g_strcmp0(args[1], "actual") != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl budget vs-actual|forecast YYYY-MM");
		return -1;
	}
	path = g_strdup_printf("/api/v1/budget_reports?period=%s&kind=%s", period, kind);
	node = venture_cli_request(cli, "GET", path, NULL, error);
	if (node == NULL)
		return -1;
	venture_cli_output(cli, node);
	return 0;
}
