/* SPDX-License-Identifier: AGPL-3.0-or-later */

/* Reads a manifest file and inlines the CSV files its "files" member
 * names, relative to the manifest, under "csv" the way the service
 * expects. A manifest that already carries "csv" is sent as is. */
static JsonNode *
venture_cli_crm_manifest(const gchar *path, GError **error)
{
	g_autoptr(JsonParser) parser = json_parser_new();
	g_autofree gchar *dir = NULL;
	g_autoptr(JsonNode) root = NULL;
	JsonObject *manifest;
	JsonObject *files;
	JsonObject *csv;
	g_autoptr(GList) names = NULL;
	GList *l;
	if (!json_parser_load_from_file(parser, path, error))
		return NULL;
	root = json_node_copy(json_parser_get_root(parser));
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s: the manifest must be a JSON object", path);
		return NULL;
	}
	manifest = json_node_get_object(root);
	files = json_object_has_member(manifest, "files") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(manifest, "files"))
		? json_object_get_object_member(manifest, "files") : NULL;
	if (files == NULL)
		return g_steal_pointer(&root);
	csv = json_object_has_member(manifest, "csv") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(manifest, "csv"))
		? json_object_get_object_member(manifest, "csv") : NULL;
	if (csv == NULL)
	{
		csv = json_object_new();
		json_object_set_object_member(manifest, "csv", csv);
	}
	dir = g_path_get_dirname(path);
	names = json_object_get_members(files);
	for (l = names; l != NULL; l = l->next)
	{
		const gchar *file = json_object_get_string_member(files, l->data);
		g_autofree gchar *full = g_path_is_absolute(file) ? g_strdup(file) : g_build_filename(dir, file, NULL);
		g_autofree gchar *text = NULL;
		if (!g_file_get_contents(full, &text, NULL, error))
			return NULL;
		json_object_set_string_member(csv, l->data, text);
	}
	json_object_remove_member(manifest, "files");
	return g_steal_pointer(&root);
}

static gint
venture_cli_command_crm(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *write_path = NULL;
	const gchar *verb = args[1];
	const gchar *target = verb != NULL ? args[2] : NULL;
	gboolean is_id = TRUE;
	guint i;
	if (verb == NULL || target == NULL || args[3] != NULL ||
		(g_strcmp0(verb, "preview") != 0 && g_strcmp0(verb, "import") != 0 &&
		 g_strcmp0(verb, "activate") != 0 && g_strcmp0(verb, "rollback") != 0))
		goto invalid;
	for (i = 0; target[i] != '\0'; i++)
		if (!g_ascii_isdigit(target[i])) is_id = FALSE;
	if (i == 0) goto invalid;
	if (!is_id)
	{
		g_autoptr(JsonNode) manifest = NULL;
		if (g_strcmp0(verb, "preview") != 0 && g_strcmp0(verb, "import") != 0)
			goto invalid;
		manifest = venture_cli_crm_manifest(target, error);
		if (manifest == NULL) return -1;
		write_path = venture_cli_write_path(cli, "/api/v1/crm_imports/preview");
		result = venture_cli_request(cli, "POST", write_path, manifest, error);
		if (result == NULL) return -1;
		if (g_strcmp0(verb, "preview") == 0)
		{
			venture_cli_output(cli, result);
			return 0;
		}
		path = g_strdup_printf("/api/v1/crm_imports/%" G_GINT64_FORMAT "/import",
			venture_json_object_get_int(json_node_get_object(result), "id", 0));
		g_clear_pointer(&result, json_node_unref);
		g_clear_pointer(&write_path, g_free);
	}
	else
	{
		if (g_strcmp0(verb, "preview") == 0) goto invalid;
		path = g_strdup_printf("/api/v1/crm_imports/%s/%s", target, verb);
	}
	write_path = venture_cli_write_path(cli, path);
	result = venture_cli_request(cli, "POST", write_path, NULL, error);
	if (result == NULL) return -1;
	venture_cli_output(cli, result);
	return 0;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		"usage: venturectl crm preview MANIFEST | crm import MANIFEST|ID | crm activate ID | crm rollback ID");
	return -1;
}
