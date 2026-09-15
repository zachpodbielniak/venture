/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_group(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;
	const gchar *period = args[2] != NULL ? args[2] : "this_month";
	if (args[1] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl group income|balance|trial [PERIOD]");
		return -1;
	}
	path = g_strdup_printf("/api/v1/group/reports?period=%s&report=consolidated_%s",
		period, g_strcmp0(args[1], "trial") == 0 ? "trial_balance" :
		(g_strcmp0(args[1], "balance") == 0 ? "balance_sheet" : "income_statement"));
	node = venture_cli_request(cli, "GET", path, NULL, error);
	if (node == NULL)
		return -1;
	venture_cli_output(cli, node);
	return 0;
}
