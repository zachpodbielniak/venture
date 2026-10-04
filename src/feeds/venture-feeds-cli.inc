/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-feeds-cli.inc - venturectl feeds sync|runs|due
 *
 * Thin calls on /api/v1/feeds. The records themselves need no verb of
 * their own -- `list data_source`, `act data_source ID sync` -- these are
 * what is not a record: queueing a sync and, with --wait, waiting for the
 * run it makes; a source's runs, newest first; and when each unit is
 * checked next. Included by src/cli/main.c, which builds without the
 * server, so nothing here needs SQLite.
 */

/* The CLI waits this long for a queued run before it says so and stops. */
#define VENTURE_CLI_FEEDS_WAIT_SECONDS (300)

/*
 * Reads the `organization_id=N` a verb with positional words may end
 * with -- feeds sync|runs|due here, market promote|watchlist|alerts
 * evaluate later in main.c -- into @out (0 when absent). The routes
 * answer for that organization, else for the active one, which for a
 * token is always the default: without it a second organization's market
 * data could not be reached from here at all. Anything else after the
 * positional words is refused with @usage rather than ignored.
 */
static gboolean
venture_cli_organization_arg(
	gchar		**args,
	guint		  first,
	const gchar	 *usage,
	gint64		 *out,
	GError		**error
){
	guint i;

	*out = 0;

	for (i = first; NULL != args[i]; i++)
	{
		gint64 parsed;

		if (!g_str_has_prefix(args[i], "organization_id=") || (0 != *out))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s (\"%s\" is not organization_id=N, or repeats it)", usage, args[i]);
			return FALSE;
		}

		if (!g_ascii_string_to_signed(args[i] + strlen("organization_id="), 10, 1, G_MAXINT64,
		                              &parsed, NULL))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "organization_id is a positive whole number, not \"%s\"",
			            args[i] + strlen("organization_id="));
			return FALSE;
		}

		*out = parsed;
	}

	return TRUE;
}

/* @path with `organization_id` added to its query string when @organization_id is set. */
static gchar *
venture_cli_organization_path(
	const gchar	*path,
	gint64		 organization_id
){
	if (organization_id <= 0)
		return g_strdup(path);

	return g_strdup_printf("%s%corganization_id=%" G_GINT64_FORMAT, path,
	                       (NULL == strchr(path, '?')) ? '?' : '&', organization_id);
}

/* The newest run's id of a source, or 0 when it has none. */
static gint64
venture_cli_feeds_last_run(
	VentureCli	 *cli,
	const gchar	 *id,
	gint64		  organization_id,
	JsonNode	**out_run,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	JsonArray *runs;

	base = g_strdup_printf("/api/v1/feeds/%s/runs?count=1", id);
	path = venture_cli_organization_path(base, organization_id);
	response = venture_cli_request(cli, "GET", path, NULL, error);

	if (NULL == response)
		return -1;

	runs = JSON_NODE_HOLDS_OBJECT(response)
		? json_object_get_array_member(json_node_get_object(response), "runs") : NULL;

	if ((NULL == runs) || (0 == json_array_get_length(runs)))
		return 0;

	if (NULL != out_run)
		*out_run = json_node_copy(json_array_get_element(runs, 0));

	return json_object_get_int_member(json_array_get_object_element(runs, 0), "id");
}

static gint
venture_cli_command_feeds(
	VentureCli	 *cli,
	gchar		**args,
	gboolean	  wait,
	GError		**error
){
	static const gchar usage[] =
		"usage: venturectl feeds sync ID [--wait] [organization_id=N] | "
		"feeds runs ID [organization_id=N] | feeds due [organization_id=N]";
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	gint64 organization_id;
	gint64 parsed;

	if ((NULL == args[1]) ||
	    ((0 != g_strcmp0(args[1], "due")) &&
	     ((NULL == args[2]) || !g_ascii_string_to_signed(args[2], 10, 1, G_MAXINT64, &parsed, NULL) ||
	      ((0 != g_strcmp0(args[1], "sync")) && (0 != g_strcmp0(args[1], "runs"))))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, usage);
		return -1;
	}

	if (!venture_cli_organization_arg(args, (0 == g_strcmp0(args[1], "due")) ? 2 : 3, usage,
	                                  &organization_id, error))
		return -1;

	if (wait && (0 != g_strcmp0(args[1], "sync")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "--wait belongs to feeds sync");
		return -1;
	}

	if (0 == g_strcmp0(args[1], "due"))
	{
		path = venture_cli_organization_path("/api/v1/feeds/due", organization_id);
		response = venture_cli_request(cli, "GET", path, NULL, error);

		if (NULL == response)
			return -1;

		venture_cli_output(cli, json_object_get_member(json_node_get_object(response), "units"));
		return 0;
	}

	if (0 == g_strcmp0(args[1], "runs"))
	{
		base = g_strdup_printf("/api/v1/feeds/%s/runs", args[2]);
		path = venture_cli_organization_path(base, organization_id);
		response = venture_cli_request(cli, "GET", path, NULL, error);

		if (NULL == response)
			return -1;

		venture_cli_output(cli, json_object_get_member(json_node_get_object(response), "runs"));
		return 0;
	}

	/* sync: the run before, so --wait knows a new one when it sees it. */
	{
		gint64 before = 0;
		gint64 deadline;

		if (wait)
		{
			before = venture_cli_feeds_last_run(cli, args[2], organization_id, NULL, error);

			if (before < 0)
				return -1;
		}

		base = g_strdup_printf("/api/v1/feeds/%s/sync", args[2]);
		path = venture_cli_organization_path(base, organization_id);
		response = venture_cli_request(cli, "POST", path, NULL, error);

		if (NULL == response)
			return -1;

		if (!wait)
		{
			venture_cli_output(cli, response);
			return 0;
		}

		/* The server never waits on a sync; the CLI may, bounded, by
		 * asking for the source's newest run once a second. */
		deadline = g_get_monotonic_time() + VENTURE_CLI_FEEDS_WAIT_SECONDS * G_TIME_SPAN_SECOND;

		while (g_get_monotonic_time() < deadline)
		{
			g_autoptr(JsonNode) run = NULL;
			gint64 latest;

			latest = venture_cli_feeds_last_run(cli, args[2], organization_id, &run, error);

			if (latest < 0)
				return -1;

			if (latest > before)
			{
				venture_cli_output(cli, run);
				return 0;
			}

			g_usleep(G_USEC_PER_SEC);
		}

		{
			g_autofree gchar *suffix = (organization_id > 0)
				? g_strdup_printf(" organization_id=%" G_GINT64_FORMAT, organization_id) : g_strdup("");

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
			            "The sync is queued, but no run was recorded within %d seconds; "
			            "see venturectl feeds runs %s%s", VENTURE_CLI_FEEDS_WAIT_SECONDS, args[2], suffix);
		}
		return -1;
	}
}
