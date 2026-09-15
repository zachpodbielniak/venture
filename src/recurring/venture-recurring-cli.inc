/* Recurring, collections and batch convenience verbs over the action API. */
static gint
venture_cli_command_recurring(VentureCli *cli, gchar **args, const gchar *as_of, gboolean dry_run, GError **error)
{
	g_autoptr(JsonNode) body = json_node_new(JSON_NODE_OBJECT);
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *path = NULL;
	const gchar *verb = args[0];
	JsonObject *object = json_object_new();
	json_node_take_object(body, object);
	if (g_strv_length(args) < 2)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: recurring run | collections run | batch invoice|expense");
		return -1;
	}
	if (g_strcmp0(verb, "recurring") == 0 && g_strcmp0(args[1], "run") == 0)
	{
		if (as_of != NULL)
			json_object_set_string_member(object, "as_of", as_of);
		json_object_set_boolean_member(object, "dry_run", dry_run);
		path = g_strdup_printf("/api/v1/recurring_schedule/0/actions/run%s", cli->stage ? "?stage=1" : "");
	}
	else if (g_strcmp0(verb, "collections") == 0 && g_strcmp0(args[1], "run") == 0)
	{
		if (as_of != NULL)
			json_object_set_string_member(object, "as_of", as_of);
		path = g_strdup_printf("/api/v1/collection_policy/0/actions/run%s", cli->stage ? "?stage=1" : "");
	}
	else if (g_strcmp0(verb, "batch") == 0 &&
		(g_strcmp0(args[1], "invoice") == 0 || g_strcmp0(args[1], "expense") == 0))
	{
		const gchar *post_text;
		gboolean post;
		/* Preserve payload bytes and the ordinary action field=value contract. */
		g_clear_pointer(&body, json_node_unref);
		body = venture_cli_values_from_args(args, 2);
		object = json_node_get_object(body);
		/* Action booleans are typed JSON, unlike the textual payload fields. */
		post_text = json_object_has_member(object, "post") ?
			json_object_get_string_member(object, "post") : "true";
		if (g_strcmp0(post_text, "true") != 0 && g_strcmp0(post_text, "false") != 0)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"post must be true or false");
			return -1;
		}
		post = g_strcmp0(post_text, "true") == 0;
		json_object_set_string_member(object, "kind", args[1]);
		json_object_set_boolean_member(object, "dry_run", dry_run);
		json_object_set_boolean_member(object, "post", post);
		path = g_strdup_printf("/api/v1/%s/0/actions/batch_create%s",
			g_strcmp0(args[1], "expense") == 0 ? "expense" : "invoice",
			cli->stage ? "?stage=1" : "");
	}
	else
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: recurring run | collections run | batch invoice|expense");
		return -1;
	}
	result = venture_cli_request(cli, "POST", path, body, error);
	if (result == NULL)
		return -1;
	venture_cli_output(cli, result);
	return 0;
}
