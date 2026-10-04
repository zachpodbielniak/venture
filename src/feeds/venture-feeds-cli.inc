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

/* The newest run's id of a source, or 0 when it has none. */
static gint64
venture_cli_feeds_last_run(
	VentureCli	 *cli,
	const gchar	 *id,
	JsonNode	**out_run,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *path = NULL;
	JsonArray *runs;

	path = g_strdup_printf("/api/v1/feeds/%s/runs?count=1", id);
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
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *path = NULL;
	gint64 parsed;

	if ((NULL == args[1]) ||
	    ((0 != g_strcmp0(args[1], "due")) &&
	     ((NULL == args[2]) || !g_ascii_string_to_signed(args[2], 10, 1, G_MAXINT64, &parsed, NULL) ||
	      ((0 != g_strcmp0(args[1], "sync")) && (0 != g_strcmp0(args[1], "runs"))))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "usage: venturectl feeds sync ID [--wait] | feeds runs ID | feeds due");
		return -1;
	}

	if (wait && (0 != g_strcmp0(args[1], "sync")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "--wait belongs to feeds sync");
		return -1;
	}

	if (0 == g_strcmp0(args[1], "due"))
	{
		response = venture_cli_request(cli, "GET", "/api/v1/feeds/due", NULL, error);

		if (NULL == response)
			return -1;

		venture_cli_output(cli, json_object_get_member(json_node_get_object(response), "units"));
		return 0;
	}

	if (0 == g_strcmp0(args[1], "runs"))
	{
		path = g_strdup_printf("/api/v1/feeds/%s/runs", args[2]);
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
			before = venture_cli_feeds_last_run(cli, args[2], NULL, error);

			if (before < 0)
				return -1;
		}

		path = g_strdup_printf("/api/v1/feeds/%s/sync", args[2]);
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

			latest = venture_cli_feeds_last_run(cli, args[2], &run, error);

			if (latest < 0)
				return -1;

			if (latest > before)
			{
				venture_cli_output(cli, run);
				return 0;
			}

			g_usleep(G_USEC_PER_SEC);
		}

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
		            "The sync is queued, but no run was recorded within %d seconds; "
		            "see venturectl feeds runs %s", VENTURE_CLI_FEEDS_WAIT_SECONDS, args[2]);
		return -1;
	}
}
