/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_fields(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	const gchar *path;
	g_autofree gchar *value_path = NULL;
	if (args[1] == NULL || (g_strcmp0(args[1], "define") != 0 && g_strcmp0(args[1], "layout") != 0 &&
		g_strcmp0(args[1], "value") != 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl fields define|layout|value field=value ...");
		return -1;
	}
	body = venture_cli_values_from_args(args, 2);
	if (g_strcmp0(args[1], "define") == 0)
		path = "/api/v1/accounting_custom_field";
	else if (g_strcmp0(args[1], "layout") == 0)
		path = "/api/v1/accounting_layout";
	else
	{
		JsonObject *values = json_node_get_object(body);
		const gchar *type = json_object_get_string_member_with_default(values, "record_type", "");
		const gchar *id = json_object_get_string_member_with_default(values, "record_id", "");
		const gchar *name = json_object_get_string_member_with_default(values, "name", "");
		const gchar *value = json_object_get_string_member_with_default(values, "value", "");
		g_autofree gchar *escaped_type = g_uri_escape_string(type, NULL, FALSE);
		g_autofree gchar *escaped_id = g_uri_escape_string(id, NULL, FALSE);
		g_autoptr(JsonNode) update = json_node_new(JSON_NODE_OBJECT);
		JsonObject *attributes = json_object_new();
		if (*type == '\0' || *id == '\0' || *name == '\0')
		{
			json_object_unref(attributes);
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"fields value requires record_type, record_id and name");
			return -1;
		}
		json_node_take_object(update, json_object_new());
		json_object_set_string_member(attributes, name, value);
		json_object_set_object_member(json_node_get_object(update), "attributes", attributes);
		value_path = g_strdup_printf("/api/v1/%s/%s", escaped_type, escaped_id);
		response = venture_cli_request(cli, "PATCH", value_path, update, error);
		if (response == NULL)
			return -1;
		venture_cli_output(cli, response);
		return 0;
	}
	response = venture_cli_request(cli, "POST", path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
