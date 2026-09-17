/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Overdue-reminder sweep over the action API: the same service the web
 * button, REST and the assistant reach. Bounded, no background thread. */
static gint
venture_cli_command_dunning(VentureCli *cli, gchar **args, GError **error)
{
	static const gchar *const typed[] = { "organization_id", "limit", "dry_run", NULL };
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *path = NULL;
	JsonObject *object;
	guint i;
	if (g_strv_length(args) < 2 || g_strcmp0(args[1], "sweep") != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: dunning sweep [as_of=DATE] [organization_id=N] [limit=N] [dry_run=true]");
		return -1;
	}
	body = venture_cli_values_from_args(args, 2);
	object = json_node_get_object(body);
	/* key=value arrives as text, and the action refuses text for an integer
	 * or boolean parameter: organization_id=1 used to fail as "invalid". */
	for (i = 0; typed[i] != NULL; i++)
	{
		JsonNode *value;
		if (!json_object_has_member(object, typed[i]))
			continue;
		value = venture_json_parse(json_object_get_string_member(object, typed[i]), error);
		if (value == NULL)
			return -1;
		json_object_set_member(object, typed[i], value);
	}
	path = g_strdup_printf("/api/v1/dunning_policy/0/actions/sweep%s", cli->stage ? "?stage=1" : "");
	result = venture_cli_request(cli, "POST", path, body, error);
	if (result == NULL)
		return -1;
	venture_cli_output(cli, result);
	return 0;
}
