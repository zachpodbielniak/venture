/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venturectl calendar show [k=v ...]: the planner's unified calendar,
 * GET /api/v1/calendar, printed as an agenda by day. Every argument goes
 * into the query string as text -- the route reads them as text, so
 * nothing needs typing -- and an option it does not know is refused by
 * name rather than sent and silently ignored.
 */
static gint
venture_cli_calendar_show(VentureCli *cli, gchar **args, GError **error)
{
	static const gchar *const known[] = {
		"view", "date", "from", "to", "kind", "venture_id", "assignee", "done",
		"organization_id", NULL
	};
	g_autoptr(GString) path = g_string_new("/api/v1/calendar");
	g_autoptr(JsonNode) node = NULL;
	JsonObject *root;
	JsonArray *items;
	JsonArray *notes;
	const gchar *current = NULL;
	gchar separator = '?';
	guint i;

	for (i = 2; args[i] != NULL; i++)
	{
		const gchar *equals = strchr(args[i], '=');
		g_autofree gchar *key = NULL;

		if (equals == NULL || equals == args[i])
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"calendar show takes key=value options, not \"%s\"", args[i]);
			return -1;
		}
		key = g_strndup(args[i], equals - args[i]);
		if (!g_strv_contains(known, key))
		{
			g_autofree gchar *list = g_strjoinv(", ", (gchar **)known);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"calendar show does not know \"%s\"; it takes %s", key, list);
			return -1;
		}
		g_string_append_c(path, separator);
		separator = '&';
		g_string_append_uri_escaped(path, key, NULL, FALSE);
		g_string_append_c(path, '=');
		g_string_append_uri_escaped(path, equals + 1, NULL, FALSE);
	}

	node = venture_cli_request(cli, "GET", path->str, NULL, error);
	if (node == NULL)
		return -1;
	if (cli->format == VENTURE_OUTPUT_FORMAT_JSON || cli->format == VENTURE_OUTPUT_FORMAT_YAML)
	{
		venture_cli_output(cli, node);
		return 0;
	}

	root = json_node_get_object(node);
	notes = json_object_get_array_member(root, "notes");
	items = json_object_get_array_member(root, "items");

	/* The notes first: a source left out is part of the answer. */
	for (i = 0; notes != NULL && i < json_array_get_length(notes); i++)
		g_print("note: %s\n", json_array_get_string_element(notes, i));

	if (items == NULL || json_array_get_length(items) == 0)
	{
		g_print("Nothing dated between %s and %s.\n",
			venture_json_object_get_string(root, "from", ""),
			venture_json_object_get_string(root, "to", ""));
		return 0;
	}

	for (i = 0; i < json_array_get_length(items); i++)
	{
		JsonObject *item = json_array_get_object_element(items, i);
		const gchar *day = venture_json_object_get_string(item, "day", "");
		const gchar *start = venture_json_object_get_string(item, "start", "");
		const gchar *detail = venture_json_object_get_string(item, "detail", NULL);
		g_autofree gchar *clock = NULL;

		if (g_strcmp0(day, current) != 0)
		{
			g_print("%s%s\n", current != NULL ? "\n" : "", day);
			current = day;
		}
		/* A timed item's start in UTC, as the API gives it; the day
		 * heading is the configured zone's. */
		clock = json_object_get_boolean_member_with_default(item, "all_day", FALSE)
			? g_strdup("all day")
			: g_strndup(strlen(start) >= 16 ? start + 11 : start, 5);
		g_print("  %-7s %-9s %s%s%s%s\n", clock,
			venture_json_object_get_string(item, "kind", ""),
			venture_json_object_get_string(item, "title", ""),
			detail != NULL ? "  -- " : "", detail != NULL ? detail : "",
			json_object_get_boolean_member_with_default(item, "overdue", FALSE) ? "  [overdue]" : "");
	}
	return 0;
}

static gint venture_cli_command_calendar(VentureCli *cli, gchar **args, GError **error)
{
	const gchar *verb = args[1];
	if (!verb) goto usage;
	/* A read: nothing to stage. */
	if (!g_strcmp0(verb, "show"))
		return venture_cli_calendar_show(cli, args, error);
	if (cli->stage) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Calendar sync cannot be staged; it is an immediate sweep"); return -1;
	}
	if (!g_strcmp0(verb, "sync")) {
		g_autoptr(JsonNode) values = venture_cli_values_from_args(args, 2);
		g_autoptr(JsonNode) result = NULL;
		JsonObject *fields = json_node_get_object(values);
		g_autofree gchar *sync_path = json_object_has_member(fields, "organization_id")
			? g_strdup_printf("/api/v1/calendar/sync?organization_id=%" G_GINT64_FORMAT, venture_json_object_get_int(fields, "organization_id", 0))
			: g_strdup("/api/v1/calendar/sync");
		result = venture_cli_request(cli, "POST", sync_path, values, error);
		if (!result) return -1;
		venture_cli_output(cli, result);
		return 0;
	}
usage:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "usage: calendar sync [organization_id=N] [limit=N] | calendar show [view=] [date=] [from=] [to=] [kind=] [venture_id=] [assignee=me] [done=1]");
	return -1;
}
