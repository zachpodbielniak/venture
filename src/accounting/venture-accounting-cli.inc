/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_accounting(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) response = NULL;
	(void)args;
	response = venture_cli_request(cli, "GET", "/api/v1/accounting/home", NULL, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
