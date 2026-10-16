/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venturectl calendar show|org [k=v ...]: the planner's unified calendar,
 * GET /api/v1/calendar printed as an agenda by day, or GET /calendar.org
 * printed as the org-mode file it is. Every argument goes into the query
 * string as text -- the routes read them as text, so nothing needs
 * typing -- and an option they do not know is refused by name rather
 * than sent and silently ignored. `calendar remind` runs the reminder
 * sweep, POST /api/v1/calendar/reminders.
 */

/* @base with every key=value from args[2] on as its query string. */
static gchar *
venture_cli_calendar_path(const gchar *base, gchar **args, const gchar *verb, GError **error)
{
	static const gchar *const known[] = {
		"view", "date", "from", "to", "kind", "venture_id", "assignee", "done",
		"organization_id", NULL
	};
	g_autoptr(GString) path = g_string_new(base);
	gchar separator = '?';
	guint i;

	for (i = 2; args[i] != NULL; i++)
	{
		const gchar *equals = strchr(args[i], '=');
		g_autofree gchar *key = NULL;

		if (equals == NULL || equals == args[i])
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"calendar %s takes key=value options, not \"%s\"", verb, args[i]);
			return NULL;
		}
		key = g_strndup(args[i], equals - args[i]);
		if (!g_strv_contains(known, key))
		{
			g_autofree gchar *list = g_strjoinv(", ", (gchar **)known);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"calendar %s does not know \"%s\"; it takes %s", verb, key, list);
			return NULL;
		}
		g_string_append_c(path, separator);
		separator = '&';
		g_string_append_uri_escaped(path, key, NULL, FALSE);
		g_string_append_c(path, '=');
		g_string_append_uri_escaped(path, equals + 1, NULL, FALSE);
	}

	return g_string_free(g_steal_pointer(&path), FALSE);
}

/* The org-agenda file, printed untouched so it can be redirected into a
 * file org-agenda-files names. */
static gint
venture_cli_calendar_org(VentureCli *cli, gchar **args, GError **error)
{
	g_autofree gchar *path = venture_cli_calendar_path("/calendar.org", args, "org", error);
	g_autofree gchar *text = NULL;

	if (path == NULL)
		return -1;
	text = venture_cli_request_text(cli, path, error);
	if (text == NULL)
		return -1;
	g_print("%s", text);
	return 0;
}

static gint
venture_cli_calendar_show(VentureCli *cli, gchar **args, GError **error)
{
	g_autofree gchar *path = venture_cli_calendar_path("/api/v1/calendar", args, "show", error);
	g_autoptr(JsonNode) node = NULL;
	JsonObject *root;
	JsonArray *items;
	JsonArray *notes;
	const gchar *current = NULL;
	guint i;

	if (path == NULL)
		return -1;
	node = venture_cli_request(cli, "GET", path, NULL, error);
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
	if (!g_strcmp0(verb, "org"))
		return venture_cli_calendar_org(cli, args, error);
	if (cli->stage) {
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Calendar sync and reminders cannot be staged; they are immediate sweeps"); return -1;
	}
	if (!g_strcmp0(verb, "remind")) {
		g_autoptr(JsonNode) result = NULL;
		/* Reminders go to people, whichever organization the event is in:
		 * the sweep takes no organization, as the lazy one on the inbox. */
		if (args[2] != NULL) {
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "calendar remind takes no options");
			return -1;
		}
		result = venture_cli_request(cli, "POST", "/api/v1/calendar/reminders", NULL, error);
		if (!result) return -1;
		venture_cli_output(cli, result);
		return 0;
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
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "usage: calendar sync [organization_id=N] [limit=N] | calendar show|org [view=] [date=] [from=] [to=] [kind=] [venture_id=] [assignee=me] [done=1] [organization_id=N] | calendar remind");
	return -1;
}

/*
 * venturectl roadmap [k=v ...]: GET /api/v1/roadmap, printed as the
 * hierarchy it is -- goals, their milestones, the epics in them -- each
 * with its tickets done of planned and its due date.
 */
static gint
venture_cli_command_roadmap(VentureCli *cli, gchar **args, GError **error)
{
	static const gchar *const known[] = {
		"goal_id", "venture_id", "from", "to", "done", "organization_id", NULL
	};
	g_autoptr(GString) path = g_string_new("/api/v1/roadmap");
	g_autoptr(JsonNode) node = NULL;
	JsonArray *rows;
	JsonArray *notes;
	gchar separator = '?';
	guint i;

	for (i = 1; args[i] != NULL; i++)
	{
		const gchar *equals = strchr(args[i], '=');
		g_autofree gchar *key = NULL;

		if (equals == NULL || equals == args[i])
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"roadmap takes key=value options, not \"%s\"", args[i]);
			return -1;
		}
		key = g_strndup(args[i], equals - args[i]);
		if (!g_strv_contains(known, key))
		{
			g_autofree gchar *list = g_strjoinv(", ", (gchar **)known);
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"roadmap does not know \"%s\"; it takes %s", key, list);
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

	notes = json_object_get_array_member(json_node_get_object(node), "notes");
	rows = json_object_get_array_member(json_node_get_object(node), "rows");

	for (i = 0; notes != NULL && i < json_array_get_length(notes); i++)
		g_print("note: %s\n", json_array_get_string_element(notes, i));

	if (rows == NULL || json_array_get_length(rows) == 0)
	{
		g_print("Nothing planned.\n");
		return 0;
	}

	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		gint64 depth = venture_json_object_get_int(row, "depth", 0);
		gint64 done = venture_json_object_get_int(row, "work_done", 0);
		gint64 total = venture_json_object_get_int(row, "work_total", 0);
		const gchar *end = venture_json_object_get_string(row, "end", NULL);
		g_autofree gchar *indent = g_strnfill((gsize)(CLAMP(depth, 0, 6) * 2), ' ');
		g_autofree gchar *work = total > 0
			? g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT, done, total)
			: g_strdup("-");

		g_print("%s%-9s #%-5" G_GINT64_FORMAT " %s  [%s, %s]%s%s\n", indent,
			venture_json_object_get_string(row, "type", ""),
			venture_json_object_get_int(row, "id", 0),
			venture_json_object_get_string(row, "title", ""),
			venture_json_object_get_string(row, "status", ""), work,
			end != NULL ? "  due " : "", end != NULL ? end : "");
	}
	return 0;
}
