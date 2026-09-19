/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint venture_cli_command_calendar(VentureCli *cli, gchar **args, GError **error)
{
	const gchar *verb = args[1];
	if (!verb) goto usage;
	if (cli->stage) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Calendar sync cannot be staged; it is an immediate sweep"); return -1;
	}
	if (!g_strcmp0(verb, "sync")) {
		g_autoptr(JsonNode) values = venture_cli_values_from_args(args, 2);
		g_autoptr(JsonNode) result = NULL;
		JsonObject *fields = json_node_get_object(values);
		g_autofree gchar *sync_path = json_object_has_member(fields, "organization_id")
			? g_strdup_printf("/api/v1/calendar/sync?organization_id=%" G_GINT64_FORMAT, venture_json_object_get_int(fields, "organization_id", 0))
			: g_strdup("/api/v1/calendar/sync");
		result = venture_cli_request(cli, "POST", sync_path, values, error);
		if (!result) return -1;
		venture_cli_output(cli, result);
		return 0;
	}
usage:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "usage: calendar sync [organization_id=N] [limit=N]");
	return -1;
}
