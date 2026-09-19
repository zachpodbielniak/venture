/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Duplicate scan, merge and dismissal over the action API: the same service
 * the Duplicates page, REST and the assistant reach. Each verb is spelled
 * as the generic act command, which types every key=value from the schema
 * (survivor and organization_id are integers) and reports a staged merge. */
static gint
venture_cli_command_act(VentureCli *cli, gchar **args, GError **error);
static gint
venture_cli_command_dedupe(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	const gchar *verb = g_strv_length(args) >= 2 ? args[1] : NULL;
	guint first;
	guint i;
	if (g_strcmp0(verb, "scan") == 0)
	{
		g_ptr_array_add(argv, g_strdup("0"));
		first = 2;
	}
	else if ((g_strcmp0(verb, "merge") == 0 || g_strcmp0(verb, "dismiss") == 0) && g_strv_length(args) >= 3)
	{
		g_ptr_array_add(argv, g_strdup(args[2]));
		first = 3;
	}
	else
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: dedupe scan [kind=company|contact] [organization_id=N] | dedupe merge ID survivor=N | dedupe dismiss ID");
		return -1;
	}
	g_ptr_array_insert(argv, 0, g_strdup("act"));
	g_ptr_array_insert(argv, 1, g_strdup("duplicate_candidate"));
	g_ptr_array_add(argv, g_strdup(verb));
	for (i = first; args[i] != NULL; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	return venture_cli_command_act(cli, (gchar **)argv->pdata, error);
}
