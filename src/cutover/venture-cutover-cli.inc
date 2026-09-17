/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venturectl cutover template SECTION
 * venturectl cutover csv source=S cutoff=D [currency=C] SECTION=@FILE ... [build_only=true]
 *
 * The CSV files are read here and sent as text; the server parses and
 * validates them exactly as it would an upload from the batch page.
 */
static gint
venture_cli_command_cutover(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	guint i;
	if (args[1] != NULL && g_strcmp0(args[1], "template") == 0 && args[2] != NULL)
	{
		g_autofree gchar *section = g_uri_escape_string(args[2], NULL, FALSE);
		g_autofree gchar *path = g_strdup_printf("/api/v1/accounting_cutovers/template/%s", section);
		g_autofree gchar *text = venture_cli_request_text(cli, path, error);
		if (text == NULL)
			return -1;
		g_print("%s", text);
		return 0;
	}
	if (args[1] == NULL || g_strcmp0(args[1], "csv") != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl cutover template SECTION | "
			"cutover csv source=S cutoff=YYYY-MM-DD [currency=C] SECTION=@FILE ... [build_only=true]");
		return -1;
	}
	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "files");
	json_builder_begin_object(builder);
	for (i = 2; args[i] != NULL; i++)
	{
		const gchar *equals = strchr(args[i], '=');
		g_autofree gchar *name = NULL;
		g_autofree gchar *contents = NULL;
		if (equals == NULL || equals[1] != '@')
			continue;
		name = g_strndup(args[i], (gsize)(equals - args[i]));
		if (!g_file_get_contents(equals + 2, &contents, NULL, error))
			return -1;
		json_builder_set_member_name(builder, name);
		json_builder_add_string_value(builder, contents);
	}
	json_builder_end_object(builder);
	for (i = 2; args[i] != NULL; i++)
	{
		const gchar *equals = strchr(args[i], '=');
		g_autofree gchar *name = NULL;
		if (equals == NULL)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"expected key=value or SECTION=@FILE, got \"%s\"", args[i]);
			return -1;
		}
		if (equals[1] == '@')
			continue;
		name = g_strndup(args[i], (gsize)(equals - args[i]));
		json_builder_set_member_name(builder, name);
		if (g_strcmp0(name, "build_only") == 0)
			json_builder_add_boolean_value(builder, g_strcmp0(equals + 1, "true") == 0 || g_strcmp0(equals + 1, "yes") == 0);
		else
			json_builder_add_string_value(builder, equals + 1);
	}
	json_builder_end_object(builder);
	body = json_builder_get_root(builder);
	response = venture_cli_request(cli, "POST", "/api/v1/accounting_cutovers/csv", body, error);
	if (response == NULL)
		return -1;
	venture_cli_output(cli, response);
	return 0;
}
