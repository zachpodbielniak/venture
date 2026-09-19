/* SPDX-License-Identifier: AGPL-3.0-or-later */
static gint
venture_cli_command_sales_tax(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(GString) path = NULL;
	g_autoptr(GBytes) bytes = NULL;
	guint i;
	if (args[1] == NULL || g_strcmp0(args[1], "export") != 0)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: venturectl sales-tax export period=PERIOD [jurisdiction=CODE] [organization_id=N] [as_of=DATE]");
		return -1;
	}
	/* The CSV is bytes the filer keeps; it is passed through untouched. */
	path = g_string_new("/api/v1/sales-tax/export");
	for (i = 2; args[i] != NULL; i++)
	{
		g_auto(GStrv) parts = g_strsplit(args[i], "=", 2);
		if (parts[1] == NULL)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"Expected key=value, got \"%s\"", args[i]);
			return -1;
		}
		g_string_append_c(path, i == 2 ? '?' : '&');
		g_string_append_uri_escaped(path, parts[0], NULL, FALSE);
		g_string_append_c(path, '=');
		g_string_append_uri_escaped(path, parts[1], NULL, FALSE);
	}
	bytes = venture_cli_request_bytes(cli, "GET", path->str, error);
	if (bytes == NULL)
		return -1;
	fwrite(g_bytes_get_data(bytes, NULL), 1, g_bytes_get_size(bytes), stdout);
	return 0;
}
