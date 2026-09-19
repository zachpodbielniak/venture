/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Backup actions over the action API: the same service the settings page,
 * REST and the assistant reach. Bounded, no background thread. */
static JsonNode *
venture_cli_backup_values(gchar **args, gint first)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	gint i;
	json_builder_begin_object(builder);
	for (i = first; args != NULL && args[i] != NULL; i++)
	{
		g_auto(GStrv) parts = g_strsplit(args[i], "=", 2);
		if (parts[0] == NULL || parts[1] == NULL)
			continue;
		json_builder_set_member_name(builder, parts[0]);
		/* The action declares these as integers; the rest are text. */
		if (g_str_equal(parts[0], "run_id") || g_str_equal(parts[0], "organization_id"))
			json_builder_add_int_value(builder, g_ascii_strtoll(parts[1], NULL, 10));
		else
			json_builder_add_string_value(builder, parts[1]);
	}
	json_builder_end_object(builder);
	return json_builder_get_root(builder);
}

static gint
venture_cli_command_backup(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *path = NULL;
	const gchar *verb = g_strv_length(args) >= 2 ? args[1] : NULL;
	if (g_strcmp0(verb, "run") == 0 && g_strv_length(args) >= 3)
	{
		body = venture_cli_backup_values(args, 3);
		path = g_strdup_printf("/api/v1/backup_schedule/%s/actions/run", args[2]);
	}
	else if (g_strcmp0(verb, "verify") == 0 && g_strv_length(args) >= 3)
	{
		body = venture_cli_backup_values(args, 3);
		path = g_strdup_printf("/api/v1/backup_run/%s/actions/verify", args[2]);
	}
	else if (g_strcmp0(verb, "restore-drill") == 0)
	{
		body = venture_cli_backup_values(args, 2);
		path = g_strdup("/api/v1/backup_run/0/actions/restore_drill");
	}
	else
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: backup run SCHEDULE_ID [as_of=DATE] | backup verify RUN_ID | backup restore-drill [run_id=N] [organization_id=N] [name=...]");
		return -1;
	}
	result = venture_cli_request(cli, "POST", path, body, error);
	if (result == NULL)
		return -1;
	venture_cli_output(cli, result);
	return 0;
}
