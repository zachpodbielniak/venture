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
	if (json_object_has_member(manifest, "files") && files == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "files must map modules to filenames");
		return NULL;
	}

	csv = json_object_has_member(manifest, "csv") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(manifest, "csv"))
		? json_object_get_object_member(manifest, "csv") : NULL;
	if (csv == NULL)
	{
		csv = json_object_new();
		json_object_set_object_member(manifest, "csv", csv);
	}
	dir = g_path_get_dirname(path);
	names = files != NULL ? json_object_get_members(files) : NULL;
	for (l = names; l != NULL; l = l->next)
	{
		JsonNode *filename = json_object_get_member(files, l->data);
		const gchar *file;
		g_autofree gchar *full = NULL;
		g_autofree gchar *text = NULL;
		if (!JSON_NODE_HOLDS_VALUE(filename) || json_node_get_value_type(filename) != G_TYPE_STRING)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "files entries must be filenames");
			return NULL;
		}
		file = json_node_get_string(filename);
		full = g_path_is_absolute(file) ? g_strdup(file) : g_build_filename(dir, file, NULL);
		if (!g_file_get_contents(full, &text, NULL, error))
			return NULL;
		json_object_set_string_member(csv, l->data, text);
	}
	{
		JsonNode *node = json_object_get_member(manifest, "attachment_files");
		JsonObject *attachments = node != NULL && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
		g_autoptr(GList) ids = attachments != NULL ? json_object_get_members(attachments) : NULL;
		g_autoptr(JsonObject) payloads = NULL;
		JsonNode *existing = json_object_get_member(manifest, "attachment_data");
		if ((node != NULL && attachments == NULL) || (existing != NULL && !JSON_NODE_HOLDS_OBJECT(existing)))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "attachment_files and attachment_data must be objects");
			return NULL;
		}
		payloads = existing != NULL ? json_object_ref(json_node_get_object(existing)) : json_object_new();
		for (l = ids; l != NULL; l = l->next)
		{
			JsonNode *filename = json_object_get_member(attachments, l->data);
			const gchar *file;
			g_autofree gchar *full = NULL;
			g_autofree gchar *content = g_malloc(8 * 1024 * 1024 + 1);
			g_autofree gchar *encoded = NULL;
			g_autoptr(GFile) input = NULL;
			g_autoptr(GFileInputStream) stream = NULL;
			gsize length;
			if (!JSON_NODE_HOLDS_VALUE(filename) || json_node_get_value_type(filename) != G_TYPE_STRING)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "attachment_files entries must be filenames");
				return NULL;
			}
			file = json_node_get_string(filename);
			full = g_path_is_absolute(file) ? g_strdup(file) : g_build_filename(dir, file, NULL);
			input = g_file_new_for_path(full);
			stream = g_file_read(input, NULL, error);
			if (stream == NULL || !g_input_stream_read_all(G_INPUT_STREAM(stream), content,
				8 * 1024 * 1024 + 1, &length, NULL, error)) return NULL;
			if (length > 8 * 1024 * 1024)
			{
				g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "CRM attachments are limited to 8 MiB each");
				return NULL;
			}
			encoded = g_base64_encode((const guchar *)content, length);
			json_object_set_string_member(payloads, l->data, encoded);
		}
		if (attachments != NULL)
			json_object_set_object_member(manifest, "attachment_data", g_steal_pointer(&payloads));
		json_object_remove_member(manifest, "attachment_files");
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
