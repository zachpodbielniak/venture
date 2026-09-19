/* SPDX-License-Identifier: AGPL-3.0-or-later
 * venturectl report packs deliver ID -- re-send a pack's last retained
 * output through the mail queue. Included by the CLI after its helpers. */
static gint venture_cli_command_report_packs(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *path = NULL;
	gchar *end = NULL;
	if (!args[2] || g_strcmp0(args[2], "deliver") || !args[3] || args[4] ||
		g_ascii_strtoll(args[3], &end, 10) <= 0 || *end) goto usage;
	if (cli->stage) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"Report pack delivery cannot be staged; it queues mail from output that already exists");
		return -1;
	}
	id = g_uri_escape_string(args[3], NULL, FALSE);
	path = g_strdup_printf("/api/v1/report_pack/%s/deliver", id);
	response = venture_cli_request(cli, "POST", path, NULL, error);
	if (!response) return -1;
	venture_cli_output(cli, response);
	return 0;
usage:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		"usage: report packs deliver ID   (re-send the pack's last retained output by email)");
	return -1;
}
