/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-web-lql.inc - Filter bars: ?q= on lists, the query bar, assist
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * Included by venture-web-server.c. A list's `q` parameter is an LQL
 * filter over the list's record type (docs/lql.org, "Filter bars"):
 *
 *   - venture_web_apply_lql_filter() runs it as the caller, through
 *     venture_web_server_new_lql_environment(), and narrows a list's
 *     VentureQuery to the ids it keeps. The list then reads, orders,
 *     pages and counts as it always did: only the selection is LQL's.
 *   - venture_web_append_query_bar() draws the bar: a plain GET form, so
 *     it works with scripting off; a problem is drawn here, server side,
 *     with the faulty text marked and a caret under it.
 *   - GET /lql/assist answers the bar's script: the highlighted text, the
 *     problem, and the completions at the cursor.
 *
 * A person's query never runs on the main loop. Every route that runs
 * one answers from a fiber (venture_web_answer_later()), and the fiber
 * hands the LQL work to a read worker (venture_web_lql_off_loop()) and
 * yields until it is done, so the listener and every other request go on
 * meanwhile. The environment is built first, in the fiber, under the
 * request's own access and tenant scopes: the reader it captures is the
 * one every read on the worker is judged by. Where the server has no read
 * workers -- hosted installs, which keep every request on the main thread
 * for their per-workspace scopes, and an in-memory database -- the work
 * runs in the fiber itself.
 */

#include "core/venture-fiber-private.h"
#include "web/venture-web-later-private.h"

/* --- Names --------------------------------------------------------------- */

/* The registry name of @entity_type ("ticket"); NULL when unregistered. */
static gchar *
venture_web_type_name(
	VentureWebServer	*self,
	GType			 entity_type
){
	g_auto(GStrv) names = NULL;
	g_autofree GType *types = NULL;
	VentureEntityRegistry *registry;
	guint n_types = 0;
	guint i;

	registry = venture_context_get_entity_registry(self->context);
	names = venture_entity_registry_list_names(registry);
	types = venture_entity_registry_list_types(registry, &n_types);

	for (i = 0; (i < n_types) && (NULL != names[i]); i++)
		if (types[i] == entity_type)
			return g_strdup(names[i]);

	return NULL;
}

/* Whether @text is NULL or white space alone. */
static gboolean
venture_web_lql_blank(const gchar *text)
{
	const gchar *p;

	if (NULL == text)
		return TRUE;

	for (p = text; '\0' != *p; p++)
		if (!g_ascii_isspace(*p))
			return FALSE;

	return TRUE;
}

/* The 1-based character column of byte @offset in @text's line. */
static guint
venture_web_lql_column(
	const gchar	*text,
	guint		 offset
){
	const gchar *line;

	line = text + offset;

	while ((line > text) && ('\n' != line[-1]))
		line--;

	return (guint)g_utf8_strlen(line, (text + offset) - line) + 1;
}

/* --- Off the main loop --------------------------------------------------------- */

/* How long past query.timeout_ms a run is cancelled outright. */
#define VENTURE_WEB_LQL_CANCEL_GRACE_MS (1000)

/* While a transaction is open below a nested loop, wake again after this. */
#define VENTURE_WEB_LQL_REARM_MS (10)

/*
 * One piece of LQL work for a read worker. Lives on the waiting fiber's
 * stack, which outlives it: the fiber does not go on until @done.
 */
typedef struct
{
	VentureDatabase		*database;	/* not a reference */
	VentureAccessSnapshot	*reader;
	VentureFiber		*fiber;
	VentureParallelFunc	 work;
	gpointer		 data;
	GError			*error;
	gint			 done;
} VentureWebLqlJob;

/* On the worker, inside the gate: a read lease, the caller's reader. */
static void
venture_web_lql_job_run(gpointer data)
{
	VentureWebLqlJob *job = data;
	VentureAccessPolicy *policy = venture_database_get_access_policy(job->database);

	venture_access_policy_bind_thread(policy);

	if (venture_database_reader_begin(job->database, &job->error))
	{
		g_autoptr(VentureAccessScope) scope = venture_access_policy_enter_snapshot(policy, job->reader);

		job->work(job->data);
		g_clear_object(&scope);
		venture_database_reader_end(job->database);
	}

	venture_access_policy_unbind_thread(policy);
}

/*
 * Wakes the waiting fiber -- but not inside somebody else's transaction:
 * a nested main loop below a writer may be the one dispatching this, and
 * the fiber resumed there would run inside it. It looks again shortly
 * instead, as the Stripe transport's waiter does.
 */
static gboolean
venture_web_lql_job_wake(gpointer data)
{
	VentureWebLqlJob *job = data;

	if (venture_database_has_transaction(job->database))
	{
		GSource *again = g_timeout_source_new(VENTURE_WEB_LQL_REARM_MS);

		g_source_set_priority(again, G_PRIORITY_DEFAULT);
		g_source_set_callback(again, venture_web_lql_job_wake, job, NULL);
		g_source_set_static_name(again, "venture-lql-wake");
		g_source_attach(again, NULL);
		g_source_unref(again);
		return G_SOURCE_REMOVE;
	}

	venture_fiber_wake(job->fiber);
	return G_SOURCE_REMOVE;
}

/* Back on the main thread, the work done. */
static void
venture_web_lql_job_done(gpointer data)
{
	VentureWebLqlJob *job = data;

	g_atomic_int_set(&job->done, 1);
	(void)venture_web_lql_job_wake(job);
}

/* The backstop: a run still going well past its time limit is cancelled. */
static gboolean
venture_web_lql_cancel(gpointer data)
{
	g_cancellable_cancel(G_CANCELLABLE(data));
	return G_SOURCE_REMOVE;
}

/*
 * Runs @work(@data) off the main loop: on a read worker, under a read
 * lease and the reader of the request the calling fiber answers, while
 * the fiber yields. @cancellable (which @work passes to LQL) is cancelled
 * once query.timeout_ms and a grace have passed, in case the run outlasts
 * LQL's own limit, which is only looked at between stages.
 *
 * Outside a fiber, or with no read workers, @work runs here.
 *
 * Returns: %FALSE, with @error set, when no read lease could be had
 */
static gboolean
venture_web_lql_off_loop(
	VentureWebServer	 *self,
	VentureParallelFunc	  work,
	gpointer		  data,
	GCancellable		 *cancellable,
	GError			**error
){
	VentureDatabase *database = venture_context_get_database(self->context);
	VentureWebLqlJob job;
	VentureFiber *fiber;
	gint64 timeout_ms = VENTURE_LQL_DEFAULT_TIMEOUT_MS;
	guint timer = 0;

	fiber = venture_fiber_current();

	if (!self->parallel_reads || (NULL == fiber))
	{
		work(data);
		return TRUE;
	}

	g_object_get(venture_context_get_config(self->context),
	             "query-timeout-ms", &timeout_ms, NULL);

	if (NULL != cancellable)
		timer = g_timeout_add_full(G_PRIORITY_DEFAULT,
			(guint)MIN(timeout_ms + VENTURE_WEB_LQL_CANCEL_GRACE_MS, (gint64)G_MAXUINT),
			venture_web_lql_cancel, g_object_ref(cancellable), g_object_unref);

	memset(&job, 0, sizeof job);
	job.database = database;
	job.reader = venture_access_policy_snapshot(venture_database_get_access_policy(database));
	job.fiber = fiber;
	job.work = work;
	job.data = data;

	/* Exactly one wake comes, from venture_web_lql_job_wake(), once the
	 * worker is done with the job. */
	venture_parallel_submit(venture_web_lql_job_run, NULL, venture_web_lql_job_done, &job);
	venture_fiber_yield();
	g_warn_if_fail(g_atomic_int_get(&job.done));

	if (0 != timer)
		g_source_remove(timer);

	venture_access_snapshot_free(job.reader);

	if (NULL != job.error)
	{
		g_propagate_error(error, job.error);
		return FALSE;
	}

	return TRUE;
}

/* Whether @request carries a filter (a non-blank q) to run. */
static gboolean
venture_web_has_lql(HtmxRequest *request)
{
	return !venture_web_lql_blank(htmx_request_get_query_param(request, "q"));
}

/*
 * A route that runs a person's query answers from a fiber. Called first
 * thing in such a route: answers with the placeholder when the route is
 * not yet in its fiber, %NULL when it is (or has no query) and should go
 * on.
 */
static HtmxResponse *
venture_web_lql_later(
	VentureWebServer	*self,
	HtmxRequest		*request,
	GHashTable		*params,
	HtmxRouteCallback	 route,
	gboolean		 always
){
	if (NULL != venture_fiber_current())
		return NULL;

	if (!always && !venture_web_has_lql(request))
		return NULL;

	return venture_web_answer_later(self, request, params, route);
}

/* --- ?q= on a list --------------------------------------------------------- */

/* venture_lql_filter_find_ids() as read-worker work. */
typedef struct
{
	LqlEnvironment		*environment;
	const gchar		*type_name;
	const gchar		*text;
	const gchar		*recheck;
	gboolean		 include_deleted;
	GCancellable		*cancellable;
	GArray			*ids;
	VentureLqlProblem	*problem;
	gboolean		 rechecked;
} VentureWebLqlFind;

static void
venture_web_lql_find(gpointer data)
{
	VentureWebLqlFind *find = data;

	find->ids = venture_lql_filter_find_ids(find->environment, find->type_name, find->text,
	                                        find->include_deleted, find->cancellable,
	                                        &find->problem);

	/* Placed again in the text a bar shows, when that is not @text
	 * alone; a problem of the whole query has no place and stays. */
	if ((NULL == find->ids) && find->problem->positioned && (NULL != find->recheck))
	{
		VentureLqlProblem *placed;

		placed = venture_lql_filter_check(find->environment, find->type_name, find->recheck);

		if (NULL != placed)
		{
			venture_lql_problem_free(find->problem);
			find->problem = placed;
			find->rechecked = TRUE;
		}
	}
}

/*
 * Narrows @query to the records the LQL filter @text keeps, read as the
 * request's caller, off the main loop (venture_web_lql_off_loop()). A
 * problem with the text is an INVALID_ARGUMENT error naming the column,
 * "q, column 8: unknown field 'staus' (did you mean 'status'?)", and
 * *out_problem (when given) holds it placed in the text -- in @recheck
 * instead, when given: the text a bar shows, which can hold more than
 * @text.
 *
 * Returns: %TRUE when @query was narrowed
 */
static gboolean
venture_web_apply_lql_filter(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	GType			  entity_type,
	VentureQuery		 *query,
	const gchar		 *text,
	gboolean		  include_deleted,
	LqlEnvironment		 *environment,
	const gchar		 *recheck,
	VentureLqlProblem	**out_problem,
	GError			**error
){
	g_autoptr(LqlEnvironment) owned = NULL;
	g_autoptr(VentureLqlProblem) problem = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autoptr(GPtrArray) values = NULL;
	g_autoptr(GCancellable) cancellable = NULL;
	g_autofree gchar *type_name = NULL;
	VentureWebLqlFind find;
	guint i;

	type_name = venture_web_type_name(self, entity_type);

	if (NULL == type_name)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "That record type cannot be filtered");
		return FALSE;
	}

	if (NULL == environment)
	{
		owned = venture_web_server_new_lql_environment(self, request, error);

		if (NULL == owned)
			return FALSE;

		environment = owned;
	}

	cancellable = g_cancellable_new();
	memset(&find, 0, sizeof find);
	find.environment = environment;
	find.type_name = type_name;
	find.text = text;
	find.recheck = recheck;
	find.include_deleted = include_deleted;
	find.cancellable = cancellable;

	if (!venture_web_lql_off_loop(self, venture_web_lql_find, &find, cancellable, error))
		return FALSE;

	ids = find.ids;
	problem = find.problem;

	if (NULL == ids)
	{
		if (problem->positioned)
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "q, column %u: %s",
			            venture_web_lql_column(find.rechecked ? recheck : text, problem->offset),
			            problem->message);
		else
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "q: %s", problem->message);

		if (NULL != out_problem)
			*out_problem = g_steal_pointer(&problem);

		return FALSE;
	}

	/* Nothing kept: a filter no row passes, since an empty IN list is
	 * refused rather than read as "none". */
	if (0 == ids->len)
		return venture_query_add_filter(query, "id", VENTURE_FILTER_OP_IS_NULL, NULL, error);

	values = g_ptr_array_new_full(ids->len, g_free);

	for (i = 0; i < ids->len; i++)
		g_ptr_array_add(values, g_strdup_printf("%" G_GINT64_FORMAT,
		                                        g_array_index(ids, gint64, i)));

	return venture_query_add_filter(query, "id", VENTURE_FILTER_OP_IN, values, error);
}

/*
 * The raw query string of @request without the parameters named in
 * @drop, '&'-joined, as it arrived otherwise. Used where a page hands its
 * own URL onward without one of its parameters (the save-view form keeps
 * `q` apart).
 */
static gchar *
venture_web_query_without(
	HtmxRequest		*request,
	const gchar * const	*drop
){
	g_autoptr(GString) out = NULL;
	g_auto(GStrv) pairs = NULL;
	const gchar *query;
	guint i;

	query = htmx_request_get_query(request);
	out = g_string_new(NULL);

	if (venture_string_is_empty(query))
		return g_string_free(g_steal_pointer(&out), FALSE);

	pairs = g_strsplit(query, "&", -1);

	for (i = 0; NULL != pairs[i]; i++)
	{
		g_autofree gchar *name = NULL;
		const gchar *equals;
		gboolean dropped = FALSE;
		guint j;

		if ('\0' == pairs[i][0])
			continue;

		equals = strchr(pairs[i], '=');
		name = (NULL != equals) ? g_strndup(pairs[i], (gsize)(equals - pairs[i]))
		                        : g_strdup(pairs[i]);

		for (j = 0; NULL != drop[j]; j++)
			dropped = dropped || (0 == g_strcmp0(name, drop[j]));

		if (dropped)
			continue;

		if (out->len > 0)
			g_string_append_c(out, '&');

		g_string_append(out, pairs[i]);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- Drawing a problem ------------------------------------------------------- */

/*
 * @text with [@offset, @offset + @length) replaced by @with: what a "did
 * you mean" link writes into the bar.
 */
static gchar *
venture_web_lql_replace(
	const gchar	*text,
	guint		 offset,
	guint		 length,
	const gchar	*with
){
	g_autoptr(GString) out = NULL;

	out = g_string_new_len(text, (gssize)offset);
	g_string_append(out, with);
	g_string_append(out, text + offset + length);

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * What sits under a filter bar: a problem -- its message, the text with
 * the faulty part marked and a caret under it, and "did you mean" links
 * that write the suggestion in -- or, when the filter is fine, the filter
 * highlighted. @base_url is the page's address up to where `q=` goes
 * ("/tickets?view=board&q="); suggestion links append to it.
 */
static void
venture_web_append_lql_status(
	GString				*html,
	LqlEnvironment			*environment,
	const gchar			*type_name,
	const gchar			*text,
	const VentureLqlProblem		*problem,
	const gchar			*base_url
){
	if (NULL == problem)
	{
		g_autofree gchar *highlighted = NULL;

		if (venture_web_lql_blank(text))
			return;

		highlighted = venture_lql_filter_highlight(environment, type_name, text, NULL);
		g_string_append(html, "<p class=\"lql-preview\"><span class=\"visually-hidden\">"
		                      "Filtering by </span><code class=\"lql-code\">");
		g_string_append(html, highlighted);
		g_string_append(html, "</code></p>");
		return;
	}

	g_string_append(html, "<div class=\"lql-problem\" role=\"alert\">"
	                      "<p class=\"lql-problem-message\"><strong>Cannot filter:</strong> ");
	venture_html_escape_append(html, problem->message);
	g_string_append(html, "</p>");

	if (problem->positioned && (NULL != text))
	{
		g_autofree gchar *line = NULL;
		const gchar *start;
		const gchar *end;
		guint column;
		guint carets;
		guint i;

		/* The line the problem is on: the whole text highlighted when
		 * it is one line, the line alone as typed when it is several. */
		start = text + problem->offset;

		while ((start > text) && ('\n' != start[-1]))
			start--;

		end = strchr(text + problem->offset, '\n');

		if (NULL == end)
			end = text + strlen(text);

		g_string_append(html, "<pre class=\"lql-code lql-caret\" aria-hidden=\"true\">");

		if ((start == text) && ('\0' == *end))
		{
			g_autofree gchar *highlighted = NULL;

			highlighted = venture_lql_filter_highlight(environment, type_name, text, problem);
			g_string_append(html, highlighted);
		}
		else
		{
			line = g_strndup(start, (gsize)(end - start));
			venture_html_escape_append(html, line);
		}

		/* Counted in characters, as the monospace line shows them. */
		column = venture_web_lql_column(text, problem->offset);
		carets = (guint)g_utf8_strlen(text + problem->offset,
		                              (gssize)MIN(problem->length,
		                                          (guint)(end - (text + problem->offset))));
		g_string_append(html, "\n<span class=\"lql-caret-line\">");

		for (i = 1; i < column; i++)
			g_string_append_c(html, ' ');

		for (i = 0; i < MAX(carets, 1u); i++)
			g_string_append_c(html, '^');

		g_string_append(html, "</span></pre>");

		/* The same place in words, for a reader the caret does not reach. */
		g_string_append_printf(html, "<p class=\"visually-hidden\">At column %u.</p>", column);
	}

	if (problem->positioned && (NULL != text) && (NULL != problem->suggestions) &&
	    (NULL != problem->suggestions[0]) && (NULL != base_url))
	{
		guint i;

		g_string_append(html, "<p class=\"lql-suggest\">Did you mean ");

		for (i = 0; (NULL != problem->suggestions[i]) && (i < 3); i++)
		{
			g_autofree gchar *fixed = NULL;
			g_autoptr(GString) href = NULL;

			fixed = venture_web_lql_replace(text, problem->offset, problem->length,
			                                problem->suggestions[i]);
			href = g_string_new(base_url);
			g_string_append_uri_escaped(href, fixed, NULL, FALSE);

			if (i > 0)
				g_string_append(html, " or ");

			g_string_append(html, "<a href=\"");
			venture_html_escape_append(html, href->str);
			g_string_append(html, "\"><code>");
			venture_html_escape_append(html, problem->suggestions[i]);
			g_string_append(html, "</code></a>");
		}

		g_string_append(html, "?</p>");
	}

	g_string_append(html, "</div>");
}

/* --- The bar ------------------------------------------------------------------- */

/*
 * A quick chip: a label and the term it adds to (or takes out of) the
 * bar. A person never has to learn the syntax for these.
 */
typedef struct
{
	const gchar	*label;
	const gchar	*clause;
} VentureWebLqlChip;

static const VentureWebLqlChip venture_web_ticket_chips[] = {
	{ "Not done",          "status not in [\"done\", \"cancelled\"]" },
	{ "Mine",              "assignee == me()" },
	{ "Unassigned",        "(is_null(assignee) or assignee == \"\")" },
	{ "Bugs",              "issue_type == \"bug\"" },
	{ "Updated this week", "updated_at >= period(\"this_week\").start" }
};

/*
 * Draws a filter bar for @type_name: a GET form posting `q` (and the
 * @hidden name, value pairs, %NULL-terminated, such as the board's view)
 * to @action; the quick @chips, each a link that writes its term into the
 * bar; and the status line (a problem, or the filter highlighted).
 * @placeholder is the example the empty input shows.
 *
 * @environment may be %NULL while @text is blank: nothing is highlighted
 * then, and the chips' terms are read from their text alone.
 * The input carries data-lql-assist, which the script reads to ask GET
 * /lql/assist for completions and highlighting while typing; without the
 * script the form submits as it is and the server draws everything.
 */
static void
venture_web_append_query_bar(
	GString				*html,
	LqlEnvironment			*environment,
	const gchar			*type_name,
	const gchar			*label,
	const gchar			*action,
	const gchar * const		*hidden,
	const VentureWebLqlChip		*chips,
	gsize				 n_chips,
	const gchar			*placeholder,
	const gchar			*text,
	const VentureLqlProblem		*problem
){
	g_autoptr(GString) base = NULL;
	g_autofree gchar *id = NULL;
	gsize i;

	id = g_strdup_printf("%s-query", type_name);

	/* The page's address with everything but q, for links that write a
	 * new filter in: chips, "did you mean", clear. */
	base = g_string_new(action);
	g_string_append_c(base, '?');

	for (i = 0; (NULL != hidden) && (NULL != hidden[i]) && (NULL != hidden[i + 1]); i += 2)
	{
		g_string_append_uri_escaped(base, hidden[i], NULL, FALSE);
		g_string_append_c(base, '=');
		g_string_append_uri_escaped(base, hidden[i + 1], NULL, FALSE);
		g_string_append_c(base, '&');
	}

	g_string_append(base, "q=");

	g_string_append(html, "<div class=\"query-bar\">"
	                      "<form class=\"query-bar-form\" method=\"get\" role=\"search\" action=\"");
	venture_html_escape_append(html, action);
	g_string_append(html, "\" aria-label=\"");
	venture_html_escape_append(html, label);
	g_string_append(html, "\">");

	for (i = 0; (NULL != hidden) && (NULL != hidden[i]) && (NULL != hidden[i + 1]); i += 2)
	{
		g_string_append(html, "<input type=\"hidden\" name=\"");
		venture_html_escape_append(html, hidden[i]);
		g_string_append(html, "\" value=\"");
		venture_html_escape_append(html, hidden[i + 1]);
		g_string_append(html, "\">");
	}

	g_string_append_printf(html,
		"<div class=\"query-bar-field\">"
		"<label class=\"visually-hidden\" for=\"%s\">", id);
	venture_html_escape_append(html, label);
	g_string_append(html, " (LQL)</label>");

	g_string_append_printf(html,
		"<input id=\"%s\" class=\"query-bar-input lql-code%s\" type=\"text\" name=\"q\" "
		"autocomplete=\"off\" autocapitalize=\"off\" spellcheck=\"false\" "
		"role=\"combobox\" aria-autocomplete=\"list\" aria-expanded=\"false\" "
		"aria-controls=\"%s-list\" aria-describedby=\"%s-status\"%s placeholder=\"",
		id, (NULL != problem) ? " has-problem" : "", id, id,
		(NULL != problem) ? " aria-invalid=\"true\"" : "");
	venture_html_escape_append(html, placeholder);
	g_string_append(html, "\" data-lql-bar data-lql-assist=\"/lql/assist?type=");
	venture_html_escape_append(html, type_name);
	g_string_append(html, "\" data-lql-base=\"");
	venture_html_escape_append(html, base->str);
	g_string_append(html, "\" value=\"");

	if (NULL != text)
		venture_html_escape_append(html, text);

	g_string_append_printf(html,
		"\"><ul id=\"%s-list\" class=\"query-bar-list\" role=\"listbox\" "
		"aria-label=\"Suggestions\" hidden></ul></div>"
		"<button class=\"btn btn-primary\" type=\"submit\">Filter</button>", id);

	if (!venture_web_lql_blank(text))
	{
		g_string_append(html, "<a class=\"btn btn-ghost\" href=\"");
		venture_html_escape_append(html, base->str);
		g_string_append(html, "\">Clear</a>");
	}

	g_string_append(html, "</form>");

	/* The chips: links, so one click works with scripting off. Each one
	 * says whether it is on, and pressing it again takes it off. */
	if (n_chips > 0)
	{
		g_string_append(html, "<div class=\"query-bar-chips\" role=\"group\" "
		                      "aria-label=\"Quick filters\">");

		for (i = 0; i < n_chips; i++)
		{
			g_autofree gchar *next = NULL;
			g_autoptr(GString) href = NULL;
			gboolean on;

			on = venture_lql_filter_has_conjunct(environment, type_name, text, chips[i].clause);
			next = venture_lql_filter_toggle(environment, type_name, text, chips[i].clause);
			href = g_string_new(base->str);
			g_string_append_uri_escaped(href, next, NULL, FALSE);

			/* A link, not a toggle button: it goes to the filtered
			 * page. Whether it is on is said in words as well as by
			 * its colour. */
			g_string_append_printf(html, "<a class=\"query-chip%s\" data-lql-chip=\"",
			                       on ? " active" : "");
			venture_html_escape_append(html, chips[i].clause);
			g_string_append(html, "\" title=\"");
			venture_html_escape_append(html, chips[i].clause);
			g_string_append(html, "\" href=\"");
			venture_html_escape_append(html, href->str);
			g_string_append(html, "\">");
			venture_html_escape_append(html, chips[i].label);
			if (on)
				g_string_append(html, "<span class=\"visually-hidden\"> (on; "
				                      "press to take it off)</span>");
			g_string_append(html, "</a>");
		}

		g_string_append(html, "</div>");
	}

	g_string_append_printf(html, "<div id=\"%s-status\" class=\"query-bar-status\" "
	                             "data-lql-status aria-live=\"polite\">", id);
	venture_web_append_lql_status(html, environment, type_name, text, problem, base->str);
	g_string_append(html, "</div></div>");
}

/* --- A generic list's bar ------------------------------------------------------- */

/*
 * The bar over a generic list, /e/@type_name: the chips its field table
 * gives (venture_lql_filter_chips()), and every other parameter of the
 * page -- field filters, a named view, the sort, a search -- carried
 * along as it is, so the bar narrows what the page shows rather than
 * replacing it. Paging is dropped: a new filter starts at page one.
 *
 * @environment may be %NULL when the page has no filter (see
 * venture_web_append_query_bar()).
 */
static void
venture_web_append_list_query_bar(
	GString			*html,
	LqlEnvironment		*environment,
	GType			 entity_type,
	const gchar		*type_name,
	const gchar		*label,
	HtmxRequest		*request,
	const VentureLqlProblem	*problem
){
	g_autoptr(GPtrArray) chips = NULL;
	g_autoptr(GPtrArray) hidden = NULL;
	g_autofree VentureWebLqlChip *borrowed = NULL;
	g_autofree gchar *action = NULL;
	GHashTable *params;
	GList *keys;
	GList *item;
	guint i;

	chips = venture_lql_filter_chips(entity_type);
	action = g_strdup_printf("/e/%s", type_name);

	/* The page's other parameters, name then value, in name order so the
	 * links the bar draws are the same for the same page. */
	hidden = g_ptr_array_new();
	params = htmx_request_get_query_params(request);
	keys = (NULL != params) ? g_list_sort(g_hash_table_get_keys(params), (GCompareFunc)g_strcmp0)
	                        : NULL;

	for (item = keys; NULL != item; item = item->next)
	{
		const gchar *name = item->data;

		if ((0 == g_strcmp0(name, "q")) || (0 == g_strcmp0(name, "page")) ||
		    (0 == g_strcmp0(name, "offset")))
			continue;

		g_ptr_array_add(hidden, item->data);
		g_ptr_array_add(hidden, g_hash_table_lookup(params, name));
	}

	g_list_free(keys);
	g_ptr_array_add(hidden, NULL);

	/* The chips as the bar takes them, borrowing their strings. */
	borrowed = g_new0(VentureWebLqlChip, MAX(chips->len, 1u));

	for (i = 0; i < chips->len; i++)
	{
		const VentureLqlChip *chip = g_ptr_array_index(chips, i);

		borrowed[i].label = chip->label;
		borrowed[i].clause = chip->clause;
	}

	venture_web_append_query_bar(html, environment, type_name, label, action,
		(const gchar * const *)hidden->pdata, borrowed, chips->len,
		(chips->len > 0) ? ((const VentureLqlChip *)g_ptr_array_index(chips, 0))->clause
		                 : "id > 100",
		htmx_request_get_query_param(request, "q"), problem);
}

/* --- GET /lql/assist ---------------------------------------------------------- */

/*
 * GET /lql/assist?type=ticket&q=TEXT&at=N[&base=URL] - what the bar's script draws
 * while a person types. N is the cursor in UTF-16 units, as a browser
 * counts it, and every offset answered is too:
 *
 *   {
 *     "status": "<the status line's HTML, as the page would draw it>",
 *     "problem": null | {"message": ..., "start": N, "end": N},
 *     "completions": [{"label", "kind", "detail", "insert", "start", "end"}]
 *   }
 *
 * Read as the caller, through the same environment a filter runs in, so a
 * value offered (a repository's name) is one they may read.
 */
/* What GET /lql/assist works out, as read-worker work. */
typedef struct
{
	LqlEnvironment		*environment;
	const gchar		*type_name;
	const gchar		*text;
	const gchar		*base;
	guint			 cursor;
	VentureLqlProblem	*problem;
	GPtrArray		*completions;
	GString			*status;
} VentureWebLqlAssist;

/*
 * Preparing runs the person's text too (an operand is evaluated while the
 * query is planned, and completion reads the repositories), so all of it
 * is worker work.
 */
static void
venture_web_lql_assist_work(gpointer data)
{
	VentureWebLqlAssist *assist = data;

	assist->problem = venture_lql_filter_check(assist->environment, assist->type_name, assist->text);
	assist->completions = venture_lql_filter_complete(assist->environment, assist->type_name,
	                                                  assist->text, assist->cursor);
	assist->status = g_string_new(NULL);
	venture_web_append_lql_status(assist->status, assist->environment, assist->type_name,
	                              assist->text, assist->problem, assist->base);
}

static HtmxResponse *
venture_web_ui_lql_assist(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 user_data
){
	VentureWebServer *self = user_data;
	g_autoptr(LqlEnvironment) environment = NULL;
	g_autoptr(VentureLqlProblem) problem = NULL;
	g_autoptr(GPtrArray) completions = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(GString) status = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) root = NULL;
	VentureWebLqlAssist assist;
	HtmxResponse *later;
	const gchar *type_name;
	const gchar *text;
	const gchar *at;
	const gchar *base;
	guint cursor;
	guint i;

	/* Always from a fiber, its work on a read worker: preparing runs
	 * what the person typed. */
	later = venture_web_lql_later(self, request, params, venture_web_ui_lql_assist, TRUE);

	if (NULL != later)
		return later;

	type_name = htmx_request_get_query_param(request, "type");
	text = htmx_request_get_query_param(request, "q");
	at = htmx_request_get_query_param(request, "at");

	if (NULL == text)
		text = "";

	if (venture_string_is_empty(type_name) ||
	    (G_TYPE_INVALID == venture_entity_registry_lookup(
		    venture_context_get_entity_registry(self->context), type_name)))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "No such record type to filter");
		return venture_web_error_response(error);
	}

	if (!g_utf8_validate(text, -1, NULL))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "The filter is not UTF-8");
		return venture_web_error_response(error);
	}

	environment = venture_web_server_new_lql_environment(self, request, &error);

	if (NULL == environment)
		return venture_web_error_response(error);

	/* The cursor arrives in UTF-16 units; LQL counts bytes. */
	cursor = venture_string_is_empty(at)
		? (guint)strlen(text)
		: lql_text_utf16_to_offset(text, (guint)g_ascii_strtoull(at, NULL, 10));

	/* The page's own address up to q=, which the bar carries
	 * (data-lql-base), for the "did you mean" links. Only a path on this
	 * server: anything else draws no links. */
	base = htmx_request_get_query_param(request, "base");

	if ((NULL == base) || ('/' != base[0]) || ('/' == base[1]) || ('\\' == base[1]))
		base = NULL;

	memset(&assist, 0, sizeof assist);
	assist.environment = environment;
	assist.type_name = type_name;
	assist.text = text;
	assist.base = base;
	assist.cursor = cursor;

	if (!venture_web_lql_off_loop(self, venture_web_lql_assist_work, &assist, NULL, &error))
		return venture_web_error_response(error);

	problem = assist.problem;
	completions = assist.completions;
	status = assist.status;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	json_builder_set_member_name(builder, "status");
	json_builder_add_string_value(builder, status->str);

	json_builder_set_member_name(builder, "problem");

	if (NULL == problem)
		json_builder_add_null_value(builder);
	else
	{
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "message");
		json_builder_add_string_value(builder, problem->message);

		if (problem->positioned)
		{
			json_builder_set_member_name(builder, "start");
			json_builder_add_int_value(builder, lql_text_offset_to_utf16(text, problem->offset));
			json_builder_set_member_name(builder, "end");
			json_builder_add_int_value(builder,
				lql_text_offset_to_utf16(text, problem->offset + problem->length));
		}

		json_builder_end_object(builder);
	}

	json_builder_set_member_name(builder, "completions");
	json_builder_begin_array(builder);

	/* A list a person reads, not a dump: the first fifty. */
	for (i = 0; (i < completions->len) && (i < 50); i++)
	{
		VentureLqlCompletion *completion = g_ptr_array_index(completions, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "label");
		json_builder_add_string_value(builder, completion->label);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, completion->kind);
		json_builder_set_member_name(builder, "detail");

		if (NULL != completion->detail)
			json_builder_add_string_value(builder, completion->detail);
		else
			json_builder_add_null_value(builder);

		json_builder_set_member_name(builder, "insert");
		json_builder_add_string_value(builder, completion->insert_text);
		json_builder_set_member_name(builder, "start");
		json_builder_add_int_value(builder, lql_text_offset_to_utf16(text, completion->offset));
		json_builder_set_member_name(builder, "end");
		json_builder_add_int_value(builder,
			lql_text_offset_to_utf16(text, completion->offset + completion->length));
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);
	root = json_builder_get_root(builder);

	return venture_web_json_response(root, 200);
}
