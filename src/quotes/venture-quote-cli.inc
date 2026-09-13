/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_quote(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *path = NULL;
	guint i;
	if (args[1] == NULL || args[2] == NULL ||
		(g_strcmp0(args[1], "send") != 0 && g_strcmp0(args[1], "accept") != 0 &&
		g_strcmp0(args[1], "decline") != 0 && g_strcmp0(args[1], "revise") != 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: quote send|accept|decline|revise ID [by=NAME] [reason=TEXT]");
		return -1;
	}
	for (i = 0; args[2][i] != '\0'; i++)
		if (!g_ascii_isdigit(args[2][i]))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Quote ID must be numeric");
			return -1;
		}
	json_builder_begin_object(builder);
	for (i = 3; args[i] != NULL; i++)
	{
		const gchar *key = g_str_has_prefix(args[i], "by=") ? "accepted_by" : "reason";
		const gchar *value = strchr(args[i], '=');
		if (value == NULL || (!g_str_has_prefix(args[i], "by=") && !g_str_has_prefix(args[i], "reason=")))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Expected by=NAME or reason=TEXT");
			return -1;
		}
		json_builder_set_member_name(builder, key);
		json_builder_add_string_value(builder, value + 1);
	}
	json_builder_end_object(builder);
	body = json_builder_get_root(builder);
	path = g_strdup_printf("/api/v1/quotes/%s/%s", args[2], args[1]);
	result = venture_cli_request(cli, "POST", path, body, error);
	if (result == NULL) return -1;
	venture_cli_output(cli, result);
	return 0;
}
