/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_sequence(VentureCli *cli, gchar **args, const gchar *as_of, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	const gchar *verb = args[1];
	if (g_strcmp0(verb, "run") == 0)
	{
		body = venture_cli_values_from_args(args, 2);
		if (as_of != NULL)
			json_object_set_string_member(json_node_get_object(body), "as_of", as_of);
		result = venture_cli_request(cli, "POST", "/api/v1/sequences/run", body, error);
	}
	else if ((g_strcmp0(verb, "enroll") == 0 || g_strcmp0(verb, "status") == 0) && args[2] != NULL && as_of == NULL)
	{
		gchar *end = NULL;
		gint64 id = g_ascii_strtoll(args[2], &end, 10);
		if (id <= 0 || end == args[2] || *end != '\0')
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Sequence and enrollment IDs must be positive integers");
			return -1;
		}
		if (g_strcmp0(verb, "enroll") == 0)
		{
			base = g_strdup_printf("/api/v1/sequence/%" G_GINT64_FORMAT "/enroll", id);
			path = venture_cli_write_path(cli, base);
			body = venture_cli_values_from_args(args, 3);
			result = venture_cli_request(cli, "POST", path, body, error);
		}
		else
		{
			g_autoptr(JsonNode) history = NULL;
			base = g_strdup_printf("/api/v1/sequence_enrollment/%" G_GINT64_FORMAT, id);
			result = venture_cli_request(cli, "GET", base, NULL, error);
			if (result == NULL)
				return -1;
			path = g_strdup_printf("/api/v1/sequence_delivery?enrollment_id=%" G_GINT64_FORMAT "&organization_id=%" G_GINT64_FORMAT,
				id, venture_json_object_get_int(json_node_get_object(result), "organization_id", 0));
			history = venture_cli_request(cli, "GET", path, NULL, error);
			if (history == NULL)
				return -1;
			json_object_set_member(json_node_get_object(result), "history", g_steal_pointer(&history));
		}
	}
	else
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"Usage: sequence enroll ID contact_id=ID | sequence run [--as-of TIMESTAMP] [organization_id=ID] | sequence status ENROLLMENT_ID");
		return -1;
	}
	if (result == NULL)
		return -1;
	venture_cli_report_staged(cli, result);
	venture_cli_output(cli, result);
	return 0;
}
