/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint venture_cli_command_mail(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL, response = NULL;
	g_autofree gchar *path = NULL;
	JsonObject *object;
	guint i;
	const gchar *verb = args[1];
	if (!verb) goto usage;
	if (!g_strcmp0(verb, "list")) {
		g_auto(GStrv) copy = g_strdupv(args);
		g_free(copy[0]); copy[0] = g_strdup("list");
		g_free(copy[1]); copy[1] = g_strdup("mail_message");
		return venture_cli_command_list(cli, copy, error);
	}
	if (cli->stage) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Mail actions cannot be staged; use --stage create mail_message to propose an enqueue"); return -1;
	}
	if (g_strcmp0(verb, "send") && g_strcmp0(verb, "test") && g_strcmp0(verb, "deliver") && g_strcmp0(verb, "retry")) goto usage;
	if (!g_strcmp0(verb, "retry") && !args[2]) goto usage;
	body = venture_cli_values_from_args(args, !g_strcmp0(verb, "retry") ? 3 : 2);
	object = json_node_get_object(body);
	if (json_object_has_member(object, "body")) {
		json_object_set_member(object, "text_body", json_node_copy(json_object_get_member(object, "body")));
		json_object_remove_member(object, "body");
	}
	for (i = 2; args[i]; i++) {
		if (!strcmp(args[i], "--html")) {
			g_autofree gchar *contents = NULL;
			if (!args[++i]) goto usage;
			if (!g_file_get_contents(args[i], &contents, NULL, error)) return -1;
			json_object_set_string_member(object, "html_body", contents);
		} else if (!strcmp(args[i], "--limit")) {
			gchar *end;
			gint64 limit;
			if (!args[++i]) goto usage;
			limit = g_ascii_strtoll(args[i], &end, 10);
			if (*end || limit < 1 || limit > 1000) goto usage;
			json_object_set_int_member(object, "limit", limit);
		}
	}
	if (!g_strcmp0(verb, "retry")) {
		g_autofree gchar *id = g_uri_escape_string(args[2], NULL, FALSE);
		path = g_strdup_printf("/api/v1/mail_messages/%s/retry", id);
	} else path = g_strdup_printf("/api/v1/mail/%s", verb);
	response = venture_cli_request(cli, "POST", path, body, error);
	if (!response) return -1;
	venture_cli_output(cli, response);
	return 0;
usage:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		"usage: mail list [state=uncertain] | mail deliver [--limit N] | mail send to=... subject=... body=... [--html FILE] | mail test to=... | mail retry ID");
	return -1;
}
