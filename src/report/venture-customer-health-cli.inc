/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Customer health sweep over the REST route the web and the assistant
 * use; the rule -- one check-in per red company, never a second while one
 * is open -- lives in venture_customer_health_sweep(). Bounded, no
 * background thread. */
static gint
venture_cli_command_customers(VentureCli *cli, gchar **args, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GString) path = NULL;
	gchar separator = '?';
	guint i;

	if ((g_strv_length(args) < 2) || (0 != g_strcmp0(args[1], "health-sweep")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: customers health-sweep [as_of=DATE] [organization_id=N] [limit=N]");
		return -1;
	}

	path = g_string_new("/api/v1/customers/health/sweep");

	for (i = 2; NULL != args[i]; i++)
	{
		g_auto(GStrv) parts = g_strsplit(args[i], "=", 2);

		if ((NULL == parts[1]) ||
		    ((0 != g_strcmp0(parts[0], "as_of")) && (0 != g_strcmp0(parts[0], "organization_id")) &&
		     (0 != g_strcmp0(parts[0], "limit"))))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"customers health-sweep takes as_of=DATE, organization_id=N and limit=N");
			return -1;
		}

		g_string_append_c(path, separator);
		g_string_append_uri_escaped(path, parts[0], NULL, FALSE);
		g_string_append_c(path, '=');
		g_string_append_uri_escaped(path, parts[1], NULL, FALSE);
		separator = '&';
	}

	body = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(body, json_object_new());
	result = venture_cli_request(cli, "POST", path->str, body, error);

	if (NULL == result)
		return -1;

	venture_cli_output(cli, result);

	return 0;
}
