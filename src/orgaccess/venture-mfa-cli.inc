/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Break-glass reset over the action API: the same service the REST route
 * and the assistant reach, so the audit row names whoever ran it. */
static gint
venture_cli_command_user(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *id = NULL;
	const gchar *who;
	if (g_strv_length(args) < 4 || g_strcmp0(args[1], "mfa") != 0 || g_strcmp0(args[2], "reset") != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: user mfa reset USER   (an id or a username; owner only)");
		return -1;
	}
	who = args[3];
	if (g_ascii_isdigit(who[0]))
		id = g_strdup(who);
	else
	{
		/* A username is looked up through the generic list route, which
		 * is owner-only for users, the same as the action itself. */
		g_autoptr(JsonNode) users = NULL;
		g_autofree gchar *escaped = g_uri_escape_string(who, NULL, FALSE);
		g_autofree gchar *list_path = g_strdup_printf("/api/v1/users?username=%s&limit=2", escaped);
		JsonArray *items;
		users = venture_cli_request(cli, "GET", list_path, NULL, error);
		if (users == NULL)
			return -1;
		items = JSON_NODE_HOLDS_OBJECT(users) && json_object_has_member(json_node_get_object(users), "records")
			? json_object_get_array_member(json_node_get_object(users), "records")
			: (JSON_NODE_HOLDS_ARRAY(users) ? json_node_get_array(users) : NULL);
		if (items == NULL || json_array_get_length(items) != 1)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "No one user named \"%s\"", who);
			return -1;
		}
		id = g_strdup_printf("%" G_GINT64_FORMAT,
			json_object_get_int_member(json_array_get_object_element(items, 0), "id"));
	}
	body = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(body, json_object_new());
	path = g_strdup_printf("/api/v1/users/%s/actions/mfa_reset", id);
	result = venture_cli_request(cli, "POST", path, body, error);
	if (result == NULL)
		return -1;
	venture_cli_output(cli, result);
	return 0;
}
