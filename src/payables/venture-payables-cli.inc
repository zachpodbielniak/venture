/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_bill(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *path = NULL;
	if (args[1] != NULL && g_strcmp0(args[1], "pay-bulk") == 0)
	{
		g_autoptr(JsonBuilder) builder = json_builder_new();
		g_auto(GStrv) parts = NULL;
		guint i;
		if (args[2] == NULL)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"usage: venturectl bill pay-bulk 1,2,3 [adapter=transfer] [date=ISO]");
			return -1;
		}
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "bills");
		json_builder_begin_array(builder);
		parts = g_strsplit(args[2], ",", 0);
		for (i = 0; parts[i] != NULL; i++)
			json_builder_add_int_value(builder, g_ascii_strtoll(parts[i], NULL, 10));
		json_builder_end_array(builder);
		{
			g_autoptr(JsonNode) extra = venture_cli_values_from_args(args, 3);
			if (extra != NULL && JSON_NODE_HOLDS_OBJECT(extra))
			{
				JsonObject *object = json_node_get_object(extra);
				GList *keys = json_object_get_members(object);
				GList *node;
				for (node = keys; node != NULL; node = node->next)
				{
					json_builder_set_member_name(builder, node->data);
					json_builder_add_string_value(builder, json_object_get_string_member(object, node->data));
				}
				g_list_free(keys);
			}
		}
		json_builder_end_object(builder);
		body = json_builder_get_root(builder);
		response = venture_cli_request(cli, "POST", "/api/v1/payables/pay", body, error);
		if (response == NULL)
			return -1;
		venture_cli_output(cli, response);
		return 0;
	}
	if (args[1] == NULL || args[2] == NULL ||
		(g_strcmp0(args[1], "approve") != 0 && g_strcmp0(args[1], "pay") != 0 && g_strcmp0(args[1], "void") != 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl bill approve|pay|void ID [field=value ...] | pay-bulk 1,2,3");
		return -1;
	}
	id = g_uri_escape_string(args[2], NULL, FALSE);
	path = g_strdup_printf("/api/v1/vendor_bill/%s/%s", id, args[1]);
	body = venture_cli_values_from_args(args, 3);
	response = venture_cli_request(cli, "POST", path, body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
