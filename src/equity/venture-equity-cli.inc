/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_equity(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) node = NULL;
	if (args[1] == NULL || args[2] == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl equity contribution|draw|loan_proceed|loan_payment|transfer AMOUNT [memo=...]");
		return -1;
	}
	body = venture_json_parse("{}", error);
	if (body == NULL)
		return -1;
	json_object_set_string_member(json_node_get_object(body), "kind", args[1]);
	json_object_set_string_member(json_node_get_object(body), "amount", args[2]);
	if (args[3] != NULL && g_str_has_prefix(args[3], "memo="))
		json_object_set_string_member(json_node_get_object(body), "memo", args[3] + 5);
	node = venture_cli_request(cli, "POST", "/api/v1/equity/post", body, error);
	if (node == NULL)
		return -1;
	venture_cli_output(cli, node);
	return 0;
}
