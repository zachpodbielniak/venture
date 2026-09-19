static gint
venture_cli_command_lead(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *write_path = NULL;
	JsonObject *options = json_object_new();
	guint i;
	json_node_take_object(body, options);
	if (args[1] == NULL || args[2] == NULL ||
		(g_strcmp0(args[1], "convert") != 0 && g_strcmp0(args[1], "reassign") != 0 &&
		 g_strcmp0(args[1], "reroute") != 0 && g_strcmp0(args[1], "rescore") != 0)) goto invalid;
	/* Re-route and rescore take no options: the rules are the input. */
	if ((g_strcmp0(args[1], "reroute") == 0 || g_strcmp0(args[1], "rescore") == 0) && args[3] != NULL) goto invalid;
	for (i = 0; args[2][i] != '\0'; i++) if (!g_ascii_isdigit(args[2][i])) goto invalid;
	if (i == 0 || g_ascii_strtoll(args[2], NULL, 10) <= 0) goto invalid;
	for (i = 3; args[i] != NULL; i++)
	{
		g_auto(GStrv) pair = g_strsplit(args[i], "=", 2);
		if (pair[1] == NULL) goto invalid;
		if (g_str_equal(pair[0], "deal"))
		{
			gboolean value;
			if (g_str_equal(pair[1], "yes") || g_str_equal(pair[1], "true")) value = TRUE;
			else if (g_str_equal(pair[1], "no") || g_str_equal(pair[1], "false")) value = FALSE;
			else goto invalid;
			json_object_set_boolean_member(options, "deal", value);
		}
		else if (g_str_equal(pair[0], "company_id") || g_str_equal(pair[0], "contact_id"))
		{
			gchar *end = NULL;
			gint64 value = g_ascii_strtoll(pair[1], &end, 10);
			if (value <= 0 || end == pair[1] || *end != '\0') goto invalid;
			json_object_set_int_member(options, pair[0], value);
		}
		else if (g_str_equal(pair[0], "owner") && g_str_equal(args[1], "reassign"))
			json_object_set_string_member(options, "owner", pair[1]);
		else goto invalid;
	}
	path = g_strdup_printf("/api/v1/leads/%s/%s", args[2], args[1]);
	write_path = venture_cli_write_path(cli, path);
	result = venture_cli_request(cli, "POST", write_path, body, error);
	if (result == NULL) return -1;
	venture_cli_output(cli, result);
	return 0;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		"usage: venturectl lead convert ID [deal=yes|no] [company_id=ID] [contact_id=ID] | lead reassign ID [owner=NAME] | leads reroute ID | leads rescore ID");
	return -1;
}
