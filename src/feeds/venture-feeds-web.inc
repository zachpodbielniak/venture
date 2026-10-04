/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-feeds-web.inc - The /feeds page and /api/v1/feeds
 *
 * Included by venture-web-server.c in a build with SQLite, and registered
 * before the generic /api/v1/:type routes, which would otherwise take
 * /api/v1/feeds for a record type that does not exist. Every handler
 * checks its own session, role and module, and reads one organization's
 * sources: the page the sidebar's pick, the API its organization_id or
 * else the active one (venture_web_request_organization()). The records
 * themselves -- data_source and data_source_run -- are served by the generic routes; these pages add
 * what is not a record: the worker's schedule, the store's upload timers
 * and the sealed credentials.
 */

/* A source in @organization_id, or a NOT_FOUND that says so. */
static VentureEntity *
venture_feeds_web_source(
	VentureWebServer	 *self,
	gint64			  organization_id,
	const gchar		 *id_text,
	GError			**error
){
	g_autoptr(VentureEntity) record = NULL;
	gint64 id;

	if ((NULL == id_text) || !g_ascii_string_to_signed(id_text, 10, 1, G_MAXINT64, &id, NULL))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A data source id is a positive number");
		return NULL;
	}

	record = venture_database_get(venture_context_get_database(self->context),
	                              VENTURE_TYPE_DATA_SOURCE, id, error);

	if ((NULL == record) || venture_entity_is_deleted(record) ||
	    (venture_entity_get_organization_id(record) != organization_id))
	{
		g_clear_error(error);
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No such data source in organization %" G_GINT64_FORMAT, organization_id);
		return NULL;
	}

	return g_steal_pointer(&record);
}

static GPtrArray *
venture_feeds_web_sources(
	VentureWebServer	 *self,
	gint64			  organization_id,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 500);

	return venture_database_find(venture_context_get_database(self->context), query, error);
}

/* The newest runs, of one source or of the organization's. */
static GPtrArray *
venture_feeds_web_runs(
	VentureWebServer	 *self,
	gint64			  organization_id,
	gint64			  source_id,
	guint			  count,
	GError			**error
){
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE_RUN);
	venture_query_set_organization(query, organization_id);

	if ((source_id > 0) &&
	    !venture_query_add_filter_int(query, "data-source-id", VENTURE_FILTER_OP_EQ, source_id, error))
		return NULL;

	venture_query_add_order(query, "finished-at", VENTURE_SORT_DESCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	venture_query_set_limit(query, count);

	return venture_database_find(venture_context_get_database(self->context), query, error);
}

static const gchar *
venture_feeds_web_status_name(VentureDataSourceRunStatus status)
{
	switch (status)
	{
	case VENTURE_DATA_SOURCE_RUN_STATUS_OK:
		return "ok";
	case VENTURE_DATA_SOURCE_RUN_STATUS_PARTIAL:
		return "partial";
	case VENTURE_DATA_SOURCE_RUN_STATUS_DEFERRED:
		return "deferred";
	case VENTURE_DATA_SOURCE_RUN_STATUS_FAILED:
	default:
		return "failed";
	}
}

/* The worker's view of one source, or NULL when it is not running it. */
static JsonObject *
venture_feeds_web_worker_source(
	JsonNode	*status,
	gint64		 id
){
	JsonArray *sources;
	guint i;

	if ((NULL == status) || !JSON_NODE_HOLDS_OBJECT(status))
		return NULL;

	sources = json_object_get_array_member(json_node_get_object(status), "sources");

	for (i = 0; (NULL != sources) && (i < json_array_get_length(sources)); i++)
	{
		JsonObject *source = json_array_get_object_element(sources, i);

		if (json_object_get_int_member(source, "id") == id)
			return source;
	}

	return NULL;
}

/* The earliest next check among a source's units, or 0. */
static gint64
venture_feeds_web_next_check(JsonObject *worker)
{
	JsonArray *units;
	gint64 earliest = 0;
	guint i;

	units = (NULL != worker) ? json_object_get_array_member(worker, "units") : NULL;

	for (i = 0; (NULL != units) && (i < json_array_get_length(units)); i++)
	{
		JsonNode *next = json_object_get_member(json_array_get_object_element(units, i), "next_check");

		if ((NULL != next) && JSON_NODE_HOLDS_VALUE(next) &&
		    ((0 == earliest) || (json_node_get_int(next) < earliest)))
			earliest = json_node_get_int(next);
	}

	return earliest;
}

static void
venture_feeds_web_append_time(
	GString	*html,
	gint64	 when
){
	g_autoptr(GDateTime) moment = NULL;
	g_autofree gchar *text = NULL;

	if (when <= 0)
	{
		g_string_append(html, "<span class=\"muted\">none</span>");
		return;
	}

	moment = g_date_time_new_from_unix_utc(when);
	text = venture_time_to_relative_string(moment);
	venture_html_escape_append(html, text);
}

/*
 * A store's upload timer: per venue, when it last changed, how often it
 * changes and when it is due again. Read through a read-only handle on the
 * main thread; the worker's writer never waits on it.
 */
static void
venture_feeds_web_append_timers(
	VentureWebServer	*self,
	GString			*html,
	VentureEntity		*source
){
	g_autoptr(VentureSeriesStore) reader = NULL;
	g_autoptr(GPtrArray) venues = NULL;
	g_autoptr(GError) error = NULL;
	VentureFeedsService *service;
	g_autofree gchar *name = NULL;
	guint i;

	service = venture_context_get_feeds_service(self->context);

	if (NULL == service)
		return;

	reader = venture_feeds_service_open_reader(service, venture_entity_get_id(source), &error);

	if (NULL == reader)
		return;

	venues = venture_series_store_list_venues(reader, &error);

	if ((NULL == venues) || (0 == venues->len))
		return;

	g_object_get(source, "name", &name, NULL);
	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Upload timer: ");
	venture_html_escape_append(html, name);
	g_string_append(html, "</h2></div><table><thead><tr><th>Venue</th><th>Last update</th>"
	                      "<th>Interval</th><th>Expected</th></tr></thead><tbody>");

	for (i = 0; (i < venues->len) && (i < 50); i++)
	{
		VentureSeriesVenueRow *venue = g_ptr_array_index(venues, i);
		VentureSeriesVenueState state;

		if (!venture_series_store_get_venue_state(reader, venue->key, &state, NULL))
			continue;

		g_string_append(html, "<tr><td>");
		venture_html_escape_append(html, !venture_string_is_empty(venue->name) ? venue->name : venue->key);
		g_string_append(html, "</td><td>");
		venture_feeds_web_append_time(html, state.found && (VENTURE_SERIES_NONE != state.last_taken_at)
		                                    ? state.last_taken_at : 0);
		g_string_append_printf(html, "</td><td>%" G_GINT64_FORMAT " min</td><td>",
		                       state.interval_seconds / 60);
		venture_feeds_web_append_time(html, state.found && (VENTURE_SERIES_NONE != state.next_expected)
		                                    ? state.next_expected : 0);
		g_string_append(html, "</td></tr>");
	}

	g_string_append(html, "</tbody></table></div>");
}

static HtmxResponse *
venture_feeds_web_page(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GString) html = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) status = NULL;
	VentureFeedsService *service;
	HtmxResponse *gate;
	gint64 organization_id;
	guint i;

	(void)params;

	gate = venture_web_ui_require_session(self, request);

	if (NULL != gate)
		return gate;

	gate = venture_web_require_module_ui(self, request, "feeds");

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	sources = venture_feeds_web_sources(self, organization_id, &error);

	if (NULL == sources)
		return venture_web_error_response(error);

	runs = venture_feeds_web_runs(self, organization_id, 0, 20, &error);

	if (NULL == runs)
		return venture_web_error_response(error);

	service = venture_context_get_feeds_service(self->context);
	status = (NULL != service) ? venture_feeds_service_dup_status(service) : NULL;

	html = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><h1>Market data feeds</h1>"
	                    "<p class=\"muted\">Sources fetched on their schedule into a store each; "
	                    "every pass is recorded as a run.</p></div>"
	                    "<a class=\"btn btn-primary\" href=\"/e/data_source/new\">Add a source</a></div>");

	if ((NULL == service) || !venture_feeds_service_is_running(service))
		g_string_append(html, "<p class=\"muted\">Nothing is being fetched: no enabled source "
		                      "runs on a schedule, and nobody has asked for a sync.</p>");

	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Sources</h2></div>");

	if (0 == sources->len)
		g_string_append(html, "<div class=\"empty\"><p class=\"muted\">No data sources yet.</p></div>");
	else
	{
		g_string_append(html, "<table><thead><tr><th>Name</th><th>Provider</th><th>Schedule</th>"
		                      "<th>Last run</th><th>Next check</th><th>Quota</th><th></th>"
		                      "</tr></thead><tbody>");

		for (i = 0; i < sources->len; i++)
		{
			VentureEntity *source = g_ptr_array_index(sources, i);
			g_autoptr(GPtrArray) last = NULL;
			g_autofree gchar *name = NULL;
			g_autofree gchar *provider = NULL;
			g_autofree gchar *schedule = NULL;
			JsonObject *worker;
			gboolean enabled;
			gint64 id;

			id = venture_entity_get_id(source);
			g_object_get(source, "name", &name, "provider", &provider, "schedule", &schedule,
			             "enabled", &enabled, NULL);
			worker = venture_feeds_web_worker_source(status, id);
			last = venture_feeds_web_runs(self, organization_id, id, 1, NULL);

			g_string_append_printf(html, "<tr><td><a href=\"/e/data_source/%" G_GINT64_FORMAT "\">", id);
			venture_html_escape_append(html, name);
			g_string_append(html, "</a></td><td>");
			venture_html_escape_append(html, provider);
			g_string_append(html, "</td><td>");
			venture_html_escape_append(html, enabled ? (venture_string_is_empty(schedule) ? "auto" : schedule)
			                                         : "off");
			g_string_append(html, "</td><td>");

			if ((NULL != last) && (last->len > 0))
			{
				VentureEntity *run = g_ptr_array_index(last, 0);
				g_autoptr(GDateTime) finished = NULL;
				g_autofree gchar *when = NULL;
				VentureDataSourceRunStatus run_status;
				gint64 rows;

				g_object_get(run, "status", &run_status, "finished-at", &finished, "rows", &rows, NULL);
				when = (NULL != finished) ? venture_time_to_relative_string(finished) : g_strdup("");
				g_string_append_printf(html, "<a href=\"/e/data_source_run/%" G_GINT64_FORMAT "\">%s</a> · ",
				                       venture_entity_get_id(run), venture_feeds_web_status_name(run_status));
				venture_html_escape_append(html, when);
				g_string_append_printf(html, " · %" G_GINT64_FORMAT " rows", rows);
			}
			else
				g_string_append(html, "<span class=\"muted\">never</span>");

			g_string_append(html, "</td><td>");
			venture_feeds_web_append_time(html, venture_feeds_web_next_check(worker));
			g_string_append(html, "</td><td>");

			if ((NULL != worker) && (json_object_get_int_member(worker, "quota_limit") > 0))
				g_string_append_printf(html, "%" G_GINT64_FORMAT " / %" G_GINT64_FORMAT " an hour",
				                       json_object_get_int_member(worker, "quota_used"),
				                       json_object_get_int_member(worker, "quota_limit"));
			else
				g_string_append(html, "<span class=\"muted\">none</span>");

			g_string_append_printf(html, "</td><td><form method=\"post\" action=\"/feeds/%" G_GINT64_FORMAT "/sync\">"
			                       "<button class=\"btn\" type=\"submit\">Sync</button></form> "
			                       "<a href=\"/feeds/%" G_GINT64_FORMAT "/credentials\">Credentials</a></td></tr>",
			                       id, id);
		}

		g_string_append(html, "</tbody></table>");
	}

	g_string_append(html, "</div>");

	for (i = 0; i < sources->len; i++)
		venture_feeds_web_append_timers(self, html, g_ptr_array_index(sources, i));

	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Recent runs</h2></div>");

	if (0 == runs->len)
		g_string_append(html, "<div class=\"empty\"><p class=\"muted\">No runs yet.</p></div>");
	else
	{
		g_string_append(html, "<table><thead><tr><th>Run</th><th>Finished</th><th>Units</th>"
		                      "<th>Rows</th><th>Error</th></tr></thead><tbody>");

		for (i = 0; i < runs->len; i++)
		{
			VentureEntity *run = g_ptr_array_index(runs, i);
			g_autoptr(GDateTime) finished = NULL;
			g_autofree gchar *run_error = NULL;
			VentureDataSourceRunStatus run_status;
			gint64 units;
			gint64 rows;

			g_object_get(run, "status", &run_status, "finished-at", &finished, "units", &units,
			             "rows", &rows, "error", &run_error, NULL);
			g_string_append_printf(html, "<tr><td><a href=\"/e/data_source_run/%" G_GINT64_FORMAT "\">#%"
			                       G_GINT64_FORMAT " %s</a></td><td>", venture_entity_get_id(run),
			                       venture_entity_get_id(run), venture_feeds_web_status_name(run_status));
			venture_feeds_web_append_time(html, (NULL != finished) ? g_date_time_to_unix(finished) : 0);
			g_string_append_printf(html, "</td><td>%" G_GINT64_FORMAT "</td><td>%" G_GINT64_FORMAT
			                       "</td><td>", units, rows);
			venture_html_escape_append(html, (NULL != run_error) ? run_error : "");
			g_string_append(html, "</td></tr>");
		}

		g_string_append(html, "</tbody></table>");
	}

	g_string_append(html, "</div>");

	return venture_web_html_response(venture_web_page(self, request, "/feeds", "Market data feeds",
	                                                  html->str), 200);
}

/* POST /feeds/:id/sync and /api/v1/feeds/:id/sync: queue, never wait. */
static HtmxResponse *
venture_feeds_web_sync(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureFeedsService *service;
	gboolean page;
	HtmxResponse *gate;
	gint64 organization_id;

	page = !g_str_has_prefix(htmx_request_get_path(request), "/api/");
	gate = page ? venture_web_ui_require_session(self, request)
	            : venture_web_api_require(self, request, VENTURE_USER_ROLE_EDITOR);

	if (NULL != gate)
		return gate;

	if (page)
	{
		g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);

		if (!venture_auth_require(self->auth, principal, VENTURE_USER_ROLE_EDITOR, &error))
			return venture_web_error_response(error);

		gate = venture_web_require_module_ui(self, request, "feeds");
	}
	else
		gate = venture_web_require_module_api(self, "feeds");

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	source = venture_feeds_web_source(self, organization_id, g_hash_table_lookup(params, "id"), &error);

	if (NULL == source)
		return venture_web_error_response(error);

	service = venture_context_get_feeds_service(self->context);

	if ((NULL == service) ||
	    !venture_feeds_service_sync(service, venture_entity_get_id(source),
	                                VENTURE_DATA_SOURCE_RUN_TRIGGER_MANUAL, &error))
	{
		if (NULL == error)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG,
			                    "Market data feeds are off (feeds.enabled)");
		return venture_web_error_response(error);
	}

	if (page)
		return venture_web_redirect_to("/feeds");

	builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder, "queued");
	json_builder_set_member_name(builder, "data_source_id");
	json_builder_add_int_value(builder, venture_entity_get_id(source));
	json_builder_end_object(builder);
	node = json_builder_get_root(builder);

	return venture_web_json_response(node, 202);
}

/* A run as the API shows it: the record's own serialisation. */
static JsonArray *
venture_feeds_web_runs_json(GPtrArray *runs)
{
	JsonArray *array;
	guint i;

	array = json_array_new();

	for (i = 0; i < runs->len; i++)
		json_array_add_element(array, venture_serializable_to_json(
			VENTURE_SERIALIZABLE(g_ptr_array_index(runs, i)), FALSE));

	return array;
}

/* GET /api/v1/feeds: every source of the organization, with its last run
 * and what the worker is doing with it. */
static HtmxResponse *
venture_feeds_web_api_list(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) status = NULL;
	g_autoptr(JsonNode) node = NULL;
	VentureFeedsService *service;
	JsonObject *root;
	JsonArray *list;
	HtmxResponse *gate;
	gint64 organization_id;
	guint i;

	(void)params;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	gate = venture_web_require_module_api(self, "feeds");

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	sources = venture_feeds_web_sources(self, organization_id, &error);

	if (NULL == sources)
		return venture_web_error_response(error);

	service = venture_context_get_feeds_service(self->context);
	status = (NULL != service) ? venture_feeds_service_dup_status(service) : NULL;
	root = json_object_new();
	list = json_array_new();

	for (i = 0; i < sources->len; i++)
	{
		VentureEntity *source = g_ptr_array_index(sources, i);
		g_autoptr(GPtrArray) last = NULL;
		JsonNode *serialized;
		JsonObject *object;
		JsonObject *worker;

		serialized = venture_serializable_to_json(VENTURE_SERIALIZABLE(source), FALSE);
		object = json_node_get_object(serialized);
		last = venture_feeds_web_runs(self, organization_id, venture_entity_get_id(source), 1, NULL);

		if ((NULL != last) && (last->len > 0))
			json_object_set_member(object, "last_run",
			                       venture_serializable_to_json(VENTURE_SERIALIZABLE(g_ptr_array_index(last, 0)),
			                                                    FALSE));
		else
			json_object_set_null_member(object, "last_run");

		worker = venture_feeds_web_worker_source(status, venture_entity_get_id(source));

		/* A fresh object, not a borrowed one: JSON-GLib shares nested
		 * containers between copies. */
		if (NULL != worker)
		{
			g_autoptr(JsonNode) wrapped = json_node_new(JSON_NODE_OBJECT);
			g_autofree gchar *text = NULL;

			json_node_set_object(wrapped, worker);
			text = json_to_string(wrapped, FALSE);
			json_object_set_member(object, "worker", json_from_string(text, NULL));
		}
		else
			json_object_set_null_member(object, "worker");

		json_array_add_element(list, serialized);
	}

	json_object_set_array_member(root, "sources", list);
	json_object_set_boolean_member(root, "running",
	                               (NULL != service) && venture_feeds_service_is_running(service));
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return venture_web_json_response(node, 200);
}

/* GET /api/v1/feeds/due: every unit and when it is checked next, soonest
 * first. A unit that never runs on its own has no time. */
static HtmxResponse *
venture_feeds_web_api_due(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GPtrArray) sources = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) status = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GPtrArray) due = NULL;
	VentureFeedsService *service;
	JsonObject *root;
	JsonArray *list;
	HtmxResponse *gate;
	gint64 organization_id;
	guint i;

	(void)params;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	gate = venture_web_require_module_api(self, "feeds");

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	sources = venture_feeds_web_sources(self, organization_id, &error);

	if (NULL == sources)
		return venture_web_error_response(error);

	service = venture_context_get_feeds_service(self->context);
	status = (NULL != service) ? venture_feeds_service_dup_status(service) : NULL;
	due = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);

	for (i = 0; i < sources->len; i++)
	{
		VentureEntity *source = g_ptr_array_index(sources, i);
		JsonObject *worker;
		JsonArray *units;
		guint j;

		worker = venture_feeds_web_worker_source(status, venture_entity_get_id(source));
		units = (NULL != worker) ? json_object_get_array_member(worker, "units") : NULL;

		for (j = 0; (NULL != units) && (j < json_array_get_length(units)); j++)
		{
			JsonObject *unit = json_array_get_object_element(units, j);
			JsonNode *next = json_object_get_member(unit, "next_check");
			JsonObject *row = json_object_new();

			json_object_set_int_member(row, "data_source_id", venture_entity_get_id(source));
			json_object_set_string_member(row, "unit", json_object_get_string_member(unit, "name"));

			if ((NULL != next) && JSON_NODE_HOLDS_VALUE(next))
			{
				g_autoptr(GDateTime) when = g_date_time_new_from_unix_utc(json_node_get_int(next));
				g_autofree gchar *text = venture_time_to_string(when);

				json_object_set_string_member(row, "next_check", text);
				json_object_set_int_member(row, "next_check_unix", json_node_get_int(next));
			}
			else
			{
				json_object_set_null_member(row, "next_check");
				json_object_set_int_member(row, "next_check_unix", G_MAXINT64);
			}

			json_object_set_int_member(row, "backoff", json_object_get_int_member(unit, "backoff"));
			g_ptr_array_add(due, row);
		}
	}

	/* Soonest first; never-scheduled last. */
	for (i = 1; i < due->len; i++)
	{
		guint j = i;

		while ((j > 0) &&
		       (json_object_get_int_member(g_ptr_array_index(due, j - 1), "next_check_unix") >
		        json_object_get_int_member(g_ptr_array_index(due, j), "next_check_unix")))
		{
			gpointer swap = due->pdata[j - 1];

			due->pdata[j - 1] = due->pdata[j];
			due->pdata[j] = swap;
			j--;
		}
	}

	root = json_object_new();
	list = json_array_new();

	for (i = 0; i < due->len; i++)
	{
		JsonObject *row = g_ptr_array_index(due, i);

		json_object_remove_member(row, "next_check_unix");
		json_array_add_object_element(list, json_object_ref(row));
	}

	json_object_set_array_member(root, "units", list);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return venture_web_json_response(node, 200);
}

/* GET /api/v1/feeds/:id/runs?count=N: a source's newest runs. */
static HtmxResponse *
venture_feeds_web_api_runs(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(GPtrArray) runs = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) node = NULL;
	const gchar *count_text;
	JsonObject *root;
	HtmxResponse *gate;
	gint64 organization_id;
	gint64 count;

	gate = venture_web_api_require(self, request, VENTURE_USER_ROLE_VIEWER);

	if (NULL != gate)
		return gate;

	gate = venture_web_require_module_api(self, "feeds");

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	source = venture_feeds_web_source(self, organization_id, g_hash_table_lookup(params, "id"), &error);

	if (NULL == source)
		return venture_web_error_response(error);

	count_text = htmx_request_get_query_param(request, "count");
	count = 20;

	if ((NULL != count_text) &&
	    !g_ascii_string_to_signed(count_text, 10, 1, 500, &count, NULL))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "count is 1 to 500");
		return venture_web_error_response(error);
	}

	runs = venture_feeds_web_runs(self, organization_id, venture_entity_get_id(source), (guint)count, &error);

	if (NULL == runs)
		return venture_web_error_response(error);

	root = json_object_new();
	json_object_set_int_member(root, "data_source_id", venture_entity_get_id(source));
	json_object_set_array_member(root, "runs", venture_feeds_web_runs_json(runs));
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return venture_web_json_response(node, 200);
}

/*
 * /feeds/:id/credentials: the provider's sensitive settings, sealed in
 * the integration store under "feed-<uuid>". Write-only, like every
 * credential form: nothing saved is ever shown back. Organization owners
 * and administrators only.
 */
static HtmxResponse *
venture_feeds_web_credentials(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	static const gint roles[] = { VENTURE_ORGANIZATION_ROLE_OWNER, VENTURE_ORGANIZATION_ROLE_ADMIN };
	VentureWebServer *self = data;
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureIntegrationService *integrations = venture_integration_service_get(database);
	g_autoptr(VentureEntity) source = NULL;
	g_autoptr(VentureIntegrationConnection) binding = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(JsonNode) sensitive = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) content = NULL;
	g_autofree gchar *provider_name = NULL;
	g_autofree gchar *key = NULL;
	VentureDataSourceProvider *provider;
	JsonObject *properties;
	JsonObject *kept;
	JsonObjectIter iter;
	const gchar *name;
	JsonNode *member;
	gint64 org;
	gint64 binding_id;
	gint64 version;
	VentureActor actor;
	HtmxResponse *gate;
	gint64 organization_id;

	gate = venture_web_ui_require_session(self, request);

	if (NULL != gate)
		return gate;

	gate = venture_web_require_module_ui(self, request, "feeds");

	if (NULL != gate)
		return gate;

	gate = venture_web_require_module_ui(self, request, "integrations");

	if (NULL != gate)
		return gate;

	{
		GBytes *bytes = htmx_request_get_body_bytes(request);

		if ((NULL != bytes) && (g_bytes_get_size(bytes) > 256 * 1024))
		{
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "A credentials request is at most 256 KiB");
			return venture_web_error_response(error);
		}
	}

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	source = venture_feeds_web_source(self, organization_id, g_hash_table_lookup(params, "id"), &error);

	if (NULL == source)
		return venture_web_error_response(error);

	org = venture_entity_get_organization_id(source);
	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_access_policy_has_organization_role(venture_database_get_access_policy(database),
	                                                 principal, org, roles, G_N_ELEMENTS(roles)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		                    "Organization integration administration is required");
		return venture_web_error_response(error);
	}

	venture_auth_to_actor(principal, &actor);
	g_object_get(source, "provider", &provider_name, NULL);
	provider = venture_data_source_provider_registry_lookup(
		venture_context_get_data_source_providers(self->context), provider_name);

	if (NULL == provider)
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No data source provider named %s is registered", provider_name);
		return venture_web_error_response(error);
	}

	/* Only the sensitive half of the provider's schema: the rest lives in
	 * the record's settings. */
	schema = venture_data_source_provider_dup_settings_schema(provider);
	properties = json_object_get_object_member(json_node_get_object(schema), "properties");
	kept = json_object_new();
	json_object_iter_init(&iter, (NULL != properties) ? properties : kept);

	while ((NULL != properties) && json_object_iter_next(&iter, &name, &member))
	{
		if (JSON_NODE_HOLDS_OBJECT(member) &&
		    venture_json_object_get_bool(json_node_get_object(member), "x-sensitive", FALSE))
			json_object_set_member(kept, name, json_node_copy(member));
	}

	sensitive = json_node_new(JSON_NODE_OBJECT);
	{
		JsonObject *wrapper = json_object_new();

		json_object_set_string_member(wrapper, "type", "object");
		json_object_set_object_member(wrapper, "properties", kept);
		json_node_take_object(sensitive, wrapper);
	}

	key = g_strconcat("feed-", venture_entity_get_uuid(source), NULL);
	binding = venture_integration_service_find(integrations, org, key, &error);

	if ((NULL != error) && !g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG))
		return venture_web_error_response(error);

	g_clear_error(&error);
	binding_id = (NULL != binding) ? venture_entity_get_id(VENTURE_ENTITY(binding)) : 0;
	version = (NULL != binding) ? venture_entity_get_version(VENTURE_ENTITY(binding)) : 0;
	content = g_string_new("<h1>Data source credentials</h1>");

	if (HTMX_METHOD_POST == htmx_request_get_method(request))
	{
		const gchar *operation = htmx_request_get_form_value(request, "operation");
		const gchar *binding_text = htmx_request_get_form_value(request, "binding_id");
		const gchar *version_text = htmx_request_get_form_value(request, "version");
		gint64 expected_binding;
		gint64 expected_version;

		if ((NULL == binding_text) || (NULL == version_text) ||
		    !g_ascii_string_to_signed(binding_text, 10, 0, G_MAXINT64, &expected_binding, NULL) ||
		    !g_ascii_string_to_signed(version_text, 10, 0, G_MAXINT64, &expected_version, NULL) ||
		    (expected_binding != binding_id) || (expected_version != version))
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT,
			                    "The credentials changed meanwhile; reload the page");
		else if (0 == g_strcmp0(operation, "configure"))
		{
			g_autoptr(JsonObject) values = venture_integration_web_values(request, sensitive, &error);
			g_autoptr(VentureIntegrationConnection) updated = NULL;

			if ((NULL != values) && (0 == json_object_get_size(values)))
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				                    "Fill in at least one credential");
			else if (NULL != values)
			{
				g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);

				json_node_set_object(node, values);
				updated = venture_integration_service_configure(integrations, org, key,
					venture_entity_get_uuid(source), "live", node, version, &actor, &error);
			}

			if (NULL != updated)
				g_string_append(content, "<p>Credentials saved and sealed. The next run uses them.</p>");
		}
		else if ((0 == g_strcmp0(operation, "disconnect")) && (NULL != binding))
		{
			if (venture_integration_service_disable(integrations, org, binding_id, version, &actor, &error))
				g_string_append(content, "<p>Credentials removed. Stored history is kept.</p>");
		}
		else
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "Choose save or remove");

		if (NULL != error)
		{
			g_string_append(content, "<div class=\"notice negative\" role=\"alert\">");
			venture_html_escape_append(content, error->message);
			g_string_append(content, "</div>");
		}

		g_clear_object(&binding);
		binding = venture_integration_service_find(integrations, org, key, NULL);
		binding_id = (NULL != binding) ? venture_entity_get_id(VENTURE_ENTITY(binding)) : 0;
		version = (NULL != binding) ? venture_entity_get_version(VENTURE_ENTITY(binding)) : 0;
	}

	g_string_append(content, (NULL != binding)
		? "<p>Credentials are set. They are write-only: nothing saved is shown here.</p>"
		: "<p>No credentials are set.</p>");

	if (0 == json_object_get_size(kept))
		g_string_append(content, "<p class=\"muted\">This provider takes no credentials.</p>");
	else
	{
		g_string_append_printf(content, "<form method=\"post\"><input type=\"hidden\" name=\"binding_id\" value=\"%"
		                       G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"version\" value=\"%"
		                       G_GINT64_FORMAT "\">", binding_id, version);
		venture_integration_web_fields(content, sensitive);
		g_string_append(content, "<button class=\"btn\" name=\"operation\" value=\"configure\">Save</button></form>");
	}

	if (NULL != binding)
		g_string_append_printf(content, "<form method=\"post\"><input type=\"hidden\" name=\"binding_id\" value=\"%"
		                       G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"version\" value=\"%"
		                       G_GINT64_FORMAT "\"><button class=\"btn\" name=\"operation\" value=\"disconnect\">"
		                       "Remove credentials</button></form>", binding_id, version);

	g_string_append(content, "<p><a href=\"/feeds\">Back to market data feeds</a></p>");

	{
		HtmxResponse *reply = venture_web_html_response(
			venture_web_page(self, request, "/feeds", "Data source credentials", content->str),
			(NULL != error) ? 400 : 200);

		htmx_response_add_header(reply, "Cache-Control", "no-store");
		return reply;
	}
}

static void
venture_feeds_web_register(VentureWebServer *self)
{
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/feeds", VENTURE_DATA_CLASS_TENANT,
	                                        VENTURE_HOSTED_ROUTE_NONE, venture_feeds_web_page, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/feeds/:id/sync", VENTURE_DATA_CLASS_TENANT,
	                                        VENTURE_HOSTED_ROUTE_NONE, venture_feeds_web_sync, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/feeds/:id/credentials", VENTURE_DATA_CLASS_TENANT,
	                                        VENTURE_HOSTED_ROUTE_NONE, venture_feeds_web_credentials, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/feeds/:id/credentials", VENTURE_DATA_CLASS_TENANT,
	                                        VENTURE_HOSTED_ROUTE_NONE, venture_feeds_web_credentials, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/feeds", VENTURE_DATA_CLASS_TENANT,
	                                        VENTURE_HOSTED_ROUTE_NONE, venture_feeds_web_api_list, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/feeds/due", VENTURE_DATA_CLASS_TENANT,
	                                        VENTURE_HOSTED_ROUTE_NONE, venture_feeds_web_api_due, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_GET, "/api/v1/feeds/:id/runs", VENTURE_DATA_CLASS_TENANT,
	                                        VENTURE_HOSTED_ROUTE_NONE, venture_feeds_web_api_runs, self);
	venture_web_server_add_classified_route(self, HTMX_METHOD_POST, "/api/v1/feeds/:id/sync", VENTURE_DATA_CLASS_TENANT,
	                                        VENTURE_HOSTED_ROUTE_NONE, venture_feeds_web_sync, self);
}
