/*
 * billing usage SUB quantity=N [key=K] [at=DATE] -- one usage report,
 * posted as a usage_record. The generic key=value helper sends every value
 * as a string; the subscription and quantity are integers on the wire, so
 * they are typed here, and anything not a whole number is refused before
 * a request is made.
 */
static gint
venture_cli_billing_usage(VentureCli *cli, gchar **args, const gchar *as_of, GError **error)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) result = NULL;
	gint64 subscription, quantity = 0;
	gboolean counted = FALSE;
	gchar *end = NULL;
	guint i;
	if (args[2] == NULL || (subscription = g_ascii_strtoll(args[2], &end, 10)) <= 0 || *end != '\0')
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"usage: billing usage SUBSCRIPTION_ID quantity=N [key=IDEMPOTENCY_KEY] [at=DATE]");
		return -1;
	}
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "subscription_id");
	json_builder_add_int_value(builder, subscription);
	if (as_of != NULL)
	{
		json_builder_set_member_name(builder, "occurred_at");
		json_builder_add_string_value(builder, as_of);
	}
	for (i = 3; args[i] != NULL; i++)
	{
		const gchar *value = strchr(args[i], '=');
		if (value == NULL)
			break;
		value++;
		if (g_str_has_prefix(args[i], "quantity="))
		{
			quantity = g_ascii_strtoll(value, &end, 10);
			if (*value == '\0' || *end != '\0' || quantity <= 0)
				break;
			json_builder_set_member_name(builder, "quantity");
			json_builder_add_int_value(builder, quantity);
			counted = TRUE;
		}
		else if (g_str_has_prefix(args[i], "key="))
		{
			json_builder_set_member_name(builder, "idempotency_key");
			json_builder_add_string_value(builder, value);
		}
		else if (g_str_has_prefix(args[i], "at="))
		{
			json_builder_set_member_name(builder, "occurred_at");
			json_builder_add_string_value(builder, value);
		}
		else
			break;
	}
	json_builder_end_object(builder);
	if (args[i] != NULL || !counted)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"billing usage takes quantity=N (a positive whole number), key=K and at=DATE");
		return -1;
	}
	body = json_builder_get_root(builder);
	result = venture_cli_request(cli, "POST", cli->stage ? "/api/v1/usage_record?stage=1" : "/api/v1/usage_record", body, error);
	if (result == NULL)
		return -1;
	venture_cli_output(cli, result);
	return 0;
}

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
			"usage: billing start company_id=N plan_price_id=N [discount_id=N | discount_code=CODE] [skip_trial=true] | billing change ID plan_price=N | billing renew [--as-of DATE] [--dry-run] | billing cancel ID | billing usage ID quantity=N [key=K]");
		return -1;
	}
	if (g_strcmp0(verb, "usage") == 0)
		return venture_cli_billing_usage(cli, args, as_of, error);
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
