/* Billing verbs transport instructions; the server owns all business rules. */
static gint
venture_cli_command_billing(VentureCli *cli, gchar **args, const gchar *as_of, gboolean dry_run, GError **error)
{
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *path = NULL;
	const gchar *verb = args[1];
	const gchar *id = NULL;
	JsonObject *object;
	gint first = 2;
	if (verb == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: billing start company_id=N plan_price_id=N | billing change ID plan_price=N | billing renew [--as-of DATE] [--dry-run] | billing cancel ID");
		return -1;
	}
	if (args[2] != NULL && g_ascii_isdigit(args[2][0]) && strchr(args[2], '=') == NULL)
	{
		gchar *end;
		if (g_ascii_strtoll(args[2], &end, 10) <= 0 || *end != '\0')
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Invalid subscription ID");
			return -1;
		}
		id = args[2];
		first++;
	}
	body = venture_cli_values_from_args(args, first);
	object = json_node_get_object(body);
	if (json_object_has_member(object, "plan_price"))
	{
		json_object_set_string_member(object, "plan_price_id", json_object_get_string_member(object, "plan_price"));
		json_object_remove_member(object, "plan_price");
	}
	if (as_of != NULL)
		json_object_set_string_member(object, "at", as_of);
	json_object_set_boolean_member(object, "dry_run", dry_run);
	if (id != NULL)
	{
		g_autofree gchar *escaped = g_uri_escape_string(verb, NULL, FALSE);
		path = g_strdup_printf("/api/v1/customer_subscriptions/%s/%s%s", id, escaped, cli->stage ? "?stage=1" : "");
	}
	else
	{
		const gchar *action = g_strcmp0(verb, "renew") == 0 ? "renew-sweep" :
			(g_strcmp0(verb, "dunning") == 0 ? "dunning-sweep" : verb);
		g_autofree gchar *escaped = g_uri_escape_string(action, NULL, FALSE);
		path = g_strdup_printf("/api/v1/billing/%s%s", escaped, cli->stage ? "?stage=1" : "");
	}
	result = venture_cli_request(cli, "POST", path, body, error);
	if (result == NULL)
		return -1;
	venture_cli_output(cli, result);
	return 0;
}
