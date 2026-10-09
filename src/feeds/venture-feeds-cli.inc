/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-feeds-cli.inc - venturectl feeds sync|push|runs|due|upkeep|upkeep-status
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

/* And this long for an upkeep: a rebuild of a store of several gigabytes
 * takes tens of minutes on a slow disk, and the passes wait for the
 * passes in flight before it starts. */
#define VENTURE_CLI_FEEDS_UPKEEP_WAIT_SECONDS (4 * 3600)

/* How long a push with --wait may take to answer: the server waits up to
 * two minutes for the run, and the session's own default (a minute) would
 * cut it off first. */
#define VENTURE_CLI_FEEDS_PUSH_TIMEOUT (150)

/* The most a push reads from a file or standard input; the server's own
 * cap (feeds.max_push_mb) is usually smaller and says so itself. */
#define VENTURE_CLI_FEEDS_PUSH_MAX ((gsize)4096 * 1048576)

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

/*
 * What --wait exits with once it has a run: the run is printed either way,
 * and a failed one is the command's failure too (exit 1), with the run's
 * own error -- a script that waited has nothing else to branch on, and a
 * malformed line read as success is a push nobody notices went nowhere.
 * Partial and deferred runs stored or will store something: exit 0.
 */
static gint
venture_cli_feeds_run_result(
	JsonNode	 *run,
	GError		**error
){
	JsonObject *object;
	const gchar *message;

	if ((NULL == run) || !JSON_NODE_HOLDS_OBJECT(run))
		return 0;

	object = json_node_get_object(run);

	if (0 != g_strcmp0(venture_json_object_get_string(object, "status", ""), "failed"))
		return 0;

	message = venture_json_object_get_string(object, "error", NULL);
	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Run %" G_GINT64_FORMAT " failed%s%s",
	            venture_json_object_get_int(object, "id", 0),
	            venture_string_is_empty(message) ? "" : ": ",
	            venture_string_is_empty(message) ? "" : message);
	return -1;
}

/* Reads a push's body: a file, or standard input for "-". */
static GBytes *
venture_cli_feeds_read_body(
	const gchar	 *name,
	GError		**error
){
	g_autoptr(GByteArray) bytes = NULL;
	guint8 chunk[65536];
	gsize got;

	if (0 != g_strcmp0(name, "-"))
	{
		g_autoptr(GMappedFile) mapped = NULL;

		mapped = g_mapped_file_new(name, FALSE, error);

		if (NULL == mapped)
			return NULL;

		return g_mapped_file_get_bytes(mapped);
	}

	bytes = g_byte_array_new();

	while (0 < (got = fread(chunk, 1, sizeof(chunk), stdin)))
	{
		if (bytes->len + got > VENTURE_CLI_FEEDS_PUSH_MAX)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Standard input is larger than any push the server takes");
			return NULL;
		}

		g_byte_array_append(bytes, chunk, (guint)got);
	}

	if (ferror(stdin))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		                    "Could not read standard input");
		return NULL;
	}

	return g_byte_array_free_to_bytes(g_steal_pointer(&bytes));
}

/*
 * feeds push ID FILE|- [--wait]: the body goes as it is, JSON lines under
 * application/x-ndjson. Nothing is parsed here: the server reads it the
 * way it reads a file_jsonl source, and a malformed line is the run's
 * failure, with its line number. With --wait the server holds the request
 * until the run is written and the run is printed, and a failed run exits
 * 1; when the server stopped waiting first (the run took longer, or other
 * requests were already waiting) the queued answer is printed and the
 * command exits 7, as feeds sync --wait does when it gives up.
 */
static gint
venture_cli_feeds_push(
	VentureCli	 *cli,
	gchar		**args,
	gboolean	  wait,
	gint64		  organization_id,
	GError		**error
){
	g_autoptr(GBytes) body = NULL;
	g_autoptr(GBytes) response = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *run;
	guint previous_timeout;
	guint status;

	body = venture_cli_feeds_read_body(args[3], error);

	if (NULL == body)
		return -1;

	base = g_strdup_printf("/api/v1/feeds/%s/push%s", args[2], wait ? "?wait=1" : "");
	path = venture_cli_organization_path(base, organization_id);

	previous_timeout = soup_session_get_timeout(cli->session);

	if (wait)
		soup_session_set_timeout(cli->session, VENTURE_CLI_FEEDS_PUSH_TIMEOUT);

	response = venture_cli_send_bytes(cli, "POST", path, body, "application/x-ndjson", &status,
	                                  error);
	soup_session_set_timeout(cli->session, previous_timeout);

	if (NULL == response)
		return -1;

	text = g_strndup(g_bytes_get_data(response, NULL), g_bytes_get_size(response));

	if (status >= 400)
	{
		venture_cli_error_from_body(text, error);
		return -1;
	}

	node = venture_json_parse(text, NULL);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "The server returned something that is not JSON (HTTP %u)", status);
		return -1;
	}

	run = json_object_get_member(json_node_get_object(node), "run");

	/* The run itself when the server waited for one, like feeds sync
	 * --wait; the queued answer otherwise, or when the run took longer. */
	venture_cli_output(cli, ((NULL != run) && JSON_NODE_HOLDS_OBJECT(run)) ? run : node);

	if (!wait)
		return 0;

	/* Waited for and not answered: the push is queued and its push_id
	 * printed, but the run it was asked to wait for is not there. */
	if (0 == g_strcmp0(venture_json_object_get_string(json_node_get_object(node), "status", ""),
	                   "queued"))
	{
		const gchar *note = venture_json_object_get_string(json_node_get_object(node), "note", NULL);

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
		            "The push is queued, but the server did not wait for its run%s%s; see "
		            "venturectl feeds runs %s", venture_string_is_empty(note) ? "" : ": ",
		            venture_string_is_empty(note) ? "" : note, args[2]);
		return -1;
	}

	return venture_cli_feeds_run_result(run, error);
}

/* GET /api/v1/feeds/ID/upkeep, and the last result's finished_at in
 * *@out_finished (0 when there is none). */
static JsonNode *
venture_cli_feeds_upkeep_state(
	VentureCli	 *cli,
	const gchar	 *id,
	gint64		  organization_id,
	gint64		 *out_finished,
	gboolean	 *out_running,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	JsonObject *object;
	JsonNode *last;
	JsonNode *running;

	base = g_strdup_printf("/api/v1/feeds/%s/upkeep", id);
	path = venture_cli_organization_path(base, organization_id);
	response = venture_cli_request(cli, "GET", path, NULL, error);

	if (NULL == response)
		return NULL;

	if (!JSON_NODE_HOLDS_OBJECT(response))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		                    "The server's upkeep answer is not a JSON object");
		return NULL;
	}

	object = json_node_get_object(response);
	last = json_object_get_member(object, "last");
	running = json_object_get_member(object, "running");
	*out_finished = ((NULL != last) && JSON_NODE_HOLDS_OBJECT(last))
		? venture_json_object_get_int(json_node_get_object(last), "finished_at", 0) : 0;
	*out_running = (NULL != running) && JSON_NODE_HOLDS_OBJECT(running);

	return g_steal_pointer(&response);
}

/*
 * feeds upkeep ID [rebuild=1] [--wait] [organization_id=N]: queues the
 * store's upkeep. With --wait it asks once every two seconds until a
 * result newer than the one before is kept and none is under way, prints
 * it, and exits 1 when it stopped on an error; it gives up (exit 7)
 * after VENTURE_CLI_FEEDS_UPKEEP_WAIT_SECONDS.
 *
 * feeds upkeep-status ID [organization_id=N]: what is under way, what the
 * last one did and the store's sizes now.
 */
static gint
venture_cli_feeds_upkeep(
	VentureCli	 *cli,
	gchar		**args,
	gboolean	  wait,
	GError		**error
){
	static const gchar usage[] =
		"usage: venturectl feeds upkeep ID [rebuild=1] [--wait] [organization_id=N] | "
		"feeds upkeep-status ID [organization_id=N]";
	g_autoptr(JsonNode) response = NULL;
	g_autoptr(JsonNode) state = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	gboolean status_only;
	gboolean rebuild;
	gboolean running;
	gint64 organization_id;
	gint64 before;
	gint64 deadline;
	guint i;

	status_only = (0 == g_strcmp0(args[1], "upkeep-status"));
	rebuild = FALSE;
	organization_id = 0;

	for (i = 3; NULL != args[i]; i++)
	{
		if (!status_only && (0 == g_strcmp0(args[i], "rebuild=1")))
			rebuild = TRUE;
		else if (!status_only && (0 == g_strcmp0(args[i], "rebuild=0")))
			rebuild = FALSE;
		else if (g_str_has_prefix(args[i], "organization_id=") && (0 == organization_id) &&
		         g_ascii_string_to_signed(args[i] + strlen("organization_id="), 10, 1, G_MAXINT64,
		                                  &organization_id, NULL))
			continue;
		else
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s (\"%s\")",
			            usage, args[i]);
			return -1;
		}
	}

	if (status_only && wait)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "--wait belongs to feeds upkeep, not upkeep-status");
		return -1;
	}

	state = venture_cli_feeds_upkeep_state(cli, args[2], organization_id, &before, &running, error);

	if (NULL == state)
		return -1;

	if (status_only)
	{
		venture_cli_output(cli, state);
		return 0;
	}

	base = g_strdup_printf("/api/v1/feeds/%s/upkeep%s", args[2], rebuild ? "?rebuild=1" : "");
	path = venture_cli_organization_path(base, organization_id);
	response = venture_cli_request(cli, "POST", path, NULL, error);

	if (NULL == response)
		return -1;

	if (!wait)
	{
		venture_cli_output(cli, response);
		return 0;
	}

	deadline = g_get_monotonic_time() + (gint64)VENTURE_CLI_FEEDS_UPKEEP_WAIT_SECONDS * G_TIME_SPAN_SECOND;

	while (g_get_monotonic_time() < deadline)
	{
		g_autoptr(JsonNode) now = NULL;
		gint64 finished;

		g_usleep(2 * G_USEC_PER_SEC);
		now = venture_cli_feeds_upkeep_state(cli, args[2], organization_id, &finished, &running, error);

		if (NULL == now)
			return -1;

		if ((finished > before) && !running)
		{
			JsonObject *last = json_object_get_object_member(json_node_get_object(now), "last");
			const gchar *failure = venture_json_object_get_string(last, "error", NULL);

			venture_cli_output(cli, json_object_get_member(json_node_get_object(now), "last"));

			if (!venture_string_is_empty(failure))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "The upkeep stopped: %s", failure);
				return -1;
			}

			return 0;
		}
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
	            "The upkeep is queued, but no result was kept within %d seconds; see "
	            "venturectl feeds upkeep-status %s", VENTURE_CLI_FEEDS_UPKEEP_WAIT_SECONDS, args[2]);
	return -1;
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
		"feeds push ID FILE|- [--wait] [organization_id=N] | "
		"feeds runs ID [organization_id=N] | feeds due [organization_id=N] | "
		"feeds upkeep ID [rebuild=1] [--wait] [organization_id=N] | "
		"feeds upkeep-status ID [organization_id=N]";
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	gint64 organization_id;
	gint64 parsed;

	if (((0 == g_strcmp0(args[1], "upkeep")) || (0 == g_strcmp0(args[1], "upkeep-status"))) &&
	    (NULL != args[2]) && g_ascii_string_to_signed(args[2], 10, 1, G_MAXINT64, &parsed, NULL))
		return venture_cli_feeds_upkeep(cli, args, wait, error);

	if ((NULL == args[1]) ||
	    ((0 != g_strcmp0(args[1], "due")) &&
	     ((NULL == args[2]) || !g_ascii_string_to_signed(args[2], 10, 1, G_MAXINT64, &parsed, NULL) ||
	      ((0 != g_strcmp0(args[1], "sync")) && (0 != g_strcmp0(args[1], "runs")) &&
	       (0 != g_strcmp0(args[1], "push"))))) ||
	    ((0 == g_strcmp0(args[1], "push")) && ((NULL == args[3]) || ('\0' == args[3][0]))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, usage);
		return -1;
	}

	if (!venture_cli_organization_arg(args,
	                                  (0 == g_strcmp0(args[1], "due")) ? 2
	                                  : (0 == g_strcmp0(args[1], "push")) ? 4 : 3,
	                                  usage, &organization_id, error))
		return -1;

	if (wait && (0 != g_strcmp0(args[1], "sync")) && (0 != g_strcmp0(args[1], "push")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "--wait belongs to feeds sync and feeds push");
		return -1;
	}

	if (0 == g_strcmp0(args[1], "push"))
		return venture_cli_feeds_push(cli, args, wait, organization_id, error);

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
				return venture_cli_feeds_run_result(run, error);
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
