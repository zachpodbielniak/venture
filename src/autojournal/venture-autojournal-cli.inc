/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_post(VentureCli *cli, gchar **args, gboolean dry_run, GError **error)
{
	g_autoptr(JsonNode) body = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(JsonNode) result = NULL;
	guint i;
	if (g_strcmp0(args[1], "backfill") != 0) goto usage;
	json_object_set_boolean_member(options, "dry_run", dry_run);
	for (i = 2; args[i] != NULL; i++) {
		if (g_str_equal(args[i], "--dry-run")) json_object_set_boolean_member(options, "dry_run", TRUE);
		else if (g_str_has_prefix(args[i], "organization_id=")) {
			gchar *end = NULL;
			gint64 org = g_ascii_strtoll(args[i] + strlen("organization_id="), &end, 10);
			if (org <= 0 || end == NULL || *end != '\0') goto usage;
			json_object_set_int_member(options, "organization_id", org);
		} else goto usage;
	}
	json_node_set_object(body, options);
	result = venture_cli_request(cli, "POST", "/api/v1/post/backfill", body, error);
	if (result == NULL) return -1;
	venture_cli_output(cli, result);
	return 0;
usage:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		"usage: venturectl post backfill [organization_id=ID] [--dry-run]");
	return -1;
}
