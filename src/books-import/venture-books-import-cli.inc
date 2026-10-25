/* SPDX-License-Identifier: AGPL-3.0-or-later */

/* A Zoho Books backup directory and settings into a manifest: every .csv
 * file in DIR by its name, and the settings given as name=value. The
 * opening balances come from a JSON file, opening=@FILE. */
static JsonNode *
venture_cli_books_manifest(gchar **args, GError **error)
{
	g_autoptr(JsonObject) manifest = json_object_new();
	JsonObject *files = json_object_new();
	g_autoptr(GDir) dir = NULL;
	const gchar *dir_path = NULL;
	const gchar *name;
	JsonNode *root;
	guint i;
	json_object_set_string_member(manifest, "source", "zoho_books");
	json_object_set_object_member(manifest, "files", files);
	for (i = 2; args[i] != NULL; i++)
	{
		const gchar *eq = strchr(args[i], '=');
		g_autofree gchar *key = NULL;
		if (eq == NULL)
		{
			if (dir_path != NULL)
				goto invalid;
			dir_path = args[i];
			continue;
		}
		key = g_strndup(args[i], (gsize)(eq - args[i]));
		if (g_strcmp0(key, "opening") == 0 || g_strcmp0(key, "accounts") == 0)
		{
			g_autoptr(JsonParser) parser = json_parser_new();
			const gchar *file = eq[1] == '@' ? eq + 2 : eq + 1;
			if (!json_parser_load_from_file(parser, file, error))
				return NULL;
			json_object_set_member(manifest, key, json_node_copy(json_parser_get_root(parser)));
		}
		else if (g_strcmp0(key, "contact_details") == 0)
			json_object_set_boolean_member(manifest, key, g_strcmp0(eq + 1, "true") == 0);
		else
			json_object_set_string_member(manifest, key, eq + 1);
	}
	if (dir_path == NULL)
		goto invalid;
	dir = g_dir_open(dir_path, 0, error);
	if (dir == NULL)
		return NULL;
	while ((name = g_dir_read_name(dir)) != NULL)
	{
		g_autofree gchar *full = NULL;
		g_autofree gchar *text = NULL;
		if (!g_str_has_suffix(name, ".csv"))
			continue;
		full = g_build_filename(dir_path, name, NULL);
		if (!g_file_get_contents(full, &text, NULL, error))
			return NULL;
		json_object_set_string_member(files, name, text);
	}
	root = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(root, manifest);
	return root;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		"usage: venturectl books preview DIR start=YYYY-MM-DD [export_date=YYYY-MM-DD] [opening=@FILE] [name=value ...]");
	return NULL;
}

static gint
venture_cli_command_books(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *write_path = NULL;
	const gchar *verb = args[1];
	static const gchar *const actions[] = { "import", "reconcile", "activate", "rollback_preflight", "rollback", NULL };
	if (g_strcmp0(verb, "preview") == 0)
	{
		g_autoptr(JsonNode) manifest = venture_cli_books_manifest(args, error);
		if (manifest == NULL)
			return -1;
		write_path = venture_cli_write_path(cli, "/api/v1/books_imports/preview");
		result = venture_cli_request(cli, "POST", write_path, manifest, error);
		if (result == NULL)
			return -1;
		venture_cli_output(cli, result);
		return 0;
	}
	if (verb == NULL || !g_strv_contains(actions, verb) || args[2] == NULL || args[3] != NULL ||
		g_ascii_strtoll(args[2], NULL, 10) <= 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl books preview DIR start=... | books import|reconcile|activate|rollback_preflight|rollback ID");
		return -1;
	}
	path = g_strdup_printf("/api/v1/books_imports/%s/%s", args[2], verb);
	write_path = venture_cli_write_path(cli, path);
	result = venture_cli_request(cli, "POST", write_path, NULL, error);
	if (result == NULL)
		return -1;
	venture_cli_output(cli, result);
	return 0;
}
