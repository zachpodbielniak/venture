/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-lql.h - VENTURE's records as LQL sources (docs/lql.org)
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * An LQL environment built for one caller: `fetch ticket`, `fetch
 * invoice`, ... for every record type that caller may open, each source
 * reading through the same query layer and access policy GET
 * /api/v1/<type> reads through, plus VENTURE's own functions (me(),
 * my_teams(), today(), period(), organization()). Nothing that reads a
 * file, runs a program or talks SQL is registered.
 *
 * This header is deliberately outside <venture.h> and the GIR: its API
 * speaks LQL's own types, and only server code links the LQL archive.
 */

#ifndef VENTURE_LQL_H
#define VENTURE_LQL_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <lql.h>

G_BEGIN_DECLS

/* The defaults of query.max_rows, query.timeout_ms and query.max_bytes. */
#define VENTURE_LQL_DEFAULT_MAX_ROWS (10000)
#define VENTURE_LQL_DEFAULT_TIMEOUT_MS (5000)
#define VENTURE_LQL_DEFAULT_MAX_BYTES (67108864)

/* The parameter `fetch TYPE, include_deleted: TRUE` reads soft-deleted
 * rows with, the same name the list API's query string uses. */
#define VENTURE_LQL_PARAM_INCLUDE_DELETED "include_deleted"

/**
 * VentureLqlTypeGate:
 * @entity_type: a record type the environment would offer as a source
 * @user_data: (closure): the data given with the gate
 * @error: (out) (optional): why the caller may not read it
 *
 * Decides whether the caller may list @entity_type at all: the role a
 * type needs (owner-only types such as user and api_token), tenant data
 * classes, support grants. The web layer passes the very check GET
 * /api/v1/<type> makes, so a type the list API refuses is never a source.
 * Module gates are applied before the gate is asked: a type of a module
 * that is off is never offered.
 *
 * Returns: %TRUE when the caller may read @entity_type
 */
typedef gboolean (*VentureLqlTypeGate)(GType     entity_type,
                                       gpointer  user_data,
                                       GError  **error);

#define VENTURE_TYPE_LQL_SOURCE (venture_lql_source_get_type())

G_DECLARE_FINAL_TYPE(VentureLqlSource, venture_lql_source, VENTURE, LQL_SOURCE, GObject)

/**
 * venture_lql_environment_new:
 * @context: the wiring: database, registry, configuration, calendar
 * @principal: the caller; every row is read as this principal
 * @organizations: (array length=n_organizations) (nullable): the selected
 *   business first, then every business beneath it; what the list API
 *   scopes an unqualified list to. %NULL or empty means no selection:
 *   every business the caller may read
 * @n_organizations: how many entries @organizations has
 * @gate: (scope call) (closure gate_data): which record types the caller
 *   may list; asked once per enabled type, while building
 * @gate_data: (nullable): data for @gate
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the environment one caller's queries run in. It holds LQL's
 * built-in commands and functions, one source per record type the caller
 * may read (named as the type is: `fetch ticket`), and VENTURE's
 * functions. Limits come from query.max_rows, query.timeout_ms and
 * query.max_bytes (LQL_LIMIT_MAX_BYTES, the string and array bytes one
 * execution may create; VENTURE #44).
 *
 * The caller's reader -- principal, any organization pin and narrowing
 * of the scope this is called in -- is captured here, and every fetch
 * reads as exactly that reader, on whatever thread it runs.
 *
 * Returns: (transfer full) (nullable): the environment, or %NULL with
 *   @error set
 */
LqlEnvironment *
venture_lql_environment_new(
	VentureContext			 *context,
	const VentureAuthPrincipal	 *principal,
	const gint64			 *organizations,
	gsize				  n_organizations,
	VentureLqlTypeGate		  gate,
	gpointer			  gate_data,
	GError				**error
);

/**
 * VentureLqlEnvironmentHook:
 * @environment: an environment venture_lql_environment_new() just built
 * @user_data: (closure): the data given with the hook
 * @error: (out) (optional): why it cannot be used
 *
 * Adds a host's own functions to every environment, on the database's
 * thread. A function it registers is evaluated on a query worker, so it
 * must be pure: no database, registry, configuration or context.
 *
 * Returns: %TRUE to use the environment
 */
typedef gboolean (*VentureLqlEnvironmentHook)(LqlEnvironment  *environment,
                                              gpointer         user_data,
                                              GError         **error);

/**
 * venture_lql_set_environment_hook:
 * @hook: (nullable) (scope forever): called on every environment built
 *   from now on; %NULL for none
 * @user_data: (closure hook): data for @hook
 *
 * Process-wide; call it on the main thread. The tests use it to add a
 * function that takes time, to show where a query is evaluated.
 */
void
venture_lql_set_environment_hook(
	VentureLqlEnvironmentHook	hook,
	gpointer			user_data
);

/**
 * venture_lql_environment_prepare_sources:
 * @environment: an environment from venture_lql_environment_new()
 *
 * Builds the schema of every source in @environment now, on the
 * database's thread. A source builds its schema from the entity registry
 * on first use; work that may ask any source for its columns -- completion,
 * describe, checking a person's text -- on a query worker needs them all
 * built first. venture_lql_query_prepare() builds only the sources its
 * query names.
 */
void
venture_lql_environment_prepare_sources(LqlEnvironment *environment);

/**
 * venture_lql_source_get_entity_type:
 * @self: a #VentureLqlSource
 *
 * Returns: the record type the source reads
 */
GType
venture_lql_source_get_entity_type(VentureLqlSource *self);

/**
 * venture_lql_environment_get_refusal:
 * @environment: an environment from venture_lql_environment_new()
 * @name: a source name, as `fetch NAME` writes it
 *
 * Why the type gate refused @name when the environment was built: the
 * very error GET /api/v1/<type> answers the same caller with. A type the
 * gate allowed, a type whose module is off and a name that is no record
 * type at all have no refusal; LQL calls them "no source named".
 *
 * Returns: (transfer none) (nullable): the refusal, or %NULL
 */
const GError *
venture_lql_environment_get_refusal(
	LqlEnvironment	*environment,
	const gchar	*name
);

/* The schema name of the document venture_lql_query_run() answers. */
#define VENTURE_LQL_QUERY_SCHEMA "venture.query/1"

/**
 * venture_lql_query_run:
 * @environment: the caller's environment (venture_lql_environment_new())
 * @text: the LQL query
 * @explain: plan the query without running it
 * @cancellable: (nullable): cancelling it fails the query
 * @error: (out) (optional): return location for a #GError
 *
 * venture_lql_query_prepare() and venture_lql_query_execute() in one, on
 * the calling thread, which must be the database's. A request surface
 * runs the two apart instead, the evaluation on a worker (docs/lql.org,
 * "Where a query runs").
 *
 * Runs one query and answers the venture.query/1 document every query
 * surface shares: `schema`, `query`, `columns` (name and LQL type),
 * `rows` (one object per row, members in column order, NULL as null,
 * Timestamps as ISO 8601 text, money as {amount, currency, exponent,
 * formatted}), `row_count`, `explain` (the plan with @explain, else null)
 * and `elapsed_ms`. POST /api/v1/query writes it, venturectl query and
 * the MCP tool venture_query print it, the query widget renders it.
 *
 * A fetch of a type the gate refused fails with that refusal
 * (venture_lql_environment_get_refusal()). LQL's errors become VENTURE's:
 * a query that does not parse, check or evaluate is
 * %VENTURE_ERROR_INVALID_ARGUMENT, a row bound (query.max_rows) is
 * %VENTURE_ERROR_VALIDATION, the time limit (query.timeout_ms) is
 * %VENTURE_ERROR_TIMEOUT. A message that has a place in the query starts
 * "LINE:COLUMN: " (venture_lql_query_error_position()).
 *
 * Returns: (transfer full) (nullable): the document, or %NULL with @error
 *   set
 */
JsonNode *
venture_lql_query_run(
	LqlEnvironment	 *environment,
	const gchar	 *text,
	gboolean	  explain,
	GCancellable	 *cancellable,
	GError		**error
);

/**
 * VentureLqlPrepared:
 *
 * A query checked and ready to run (venture_lql_query_prepare()).
 */
typedef struct _VentureLqlPrepared VentureLqlPrepared;

/**
 * venture_lql_query_prepare:
 * @environment: the caller's environment (venture_lql_environment_new())
 * @text: the LQL query
 * @explain: plan the query without running it
 * @error: (out) (optional): return location for a #GError
 *
 * Everything a query needs of the database's thread, done on it: the text
 * parsed within LQL's limits, every fetch checked against the caller's
 * refusals, and the schema of every source it names built from the entity
 * registry. What is left -- planning, evaluation, the document -- touches
 * no database, registry or configuration, and may run on a worker with
 * venture_lql_query_execute().
 *
 * Returns: (transfer full) (nullable): the prepared query, or %NULL with
 *   @error set as venture_lql_query_run() sets it
 */
VentureLqlPrepared *
venture_lql_query_prepare(
	LqlEnvironment	 *environment,
	const gchar	 *text,
	gboolean	  explain,
	GError		**error
);

/**
 * venture_lql_query_execute:
 * @prepared: a query from venture_lql_query_prepare()
 * @cancellable: (nullable): cancelling it fails the query
 * @error: (out) (optional): return location for a #GError
 *
 * Runs @prepared and answers its venture.query/1 document. Safe on any
 * thread, provided a thread other than the database's has a read dispatch
 * (venture_lql_set_read_dispatch()): every fetch is then handed back to be
 * read there. Cancelling stops the evaluation at its next step, and a
 * fetch not yet read is never read; the error is %VENTURE_ERROR_TIMEOUT.
 * One prepared query is executed by one thread at a time.
 *
 * Returns: (transfer full) (nullable): the document, or %NULL with @error
 *   set
 */
JsonNode *
venture_lql_query_execute(
	VentureLqlPrepared	 *prepared,
	GCancellable		 *cancellable,
	GError			**error
);

/**
 * venture_lql_prepared_free:
 * @prepared: (nullable): a prepared query
 */
void
venture_lql_prepared_free(VentureLqlPrepared *prepared);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureLqlPrepared, venture_lql_prepared_free)

/**
 * VentureLqlRead:
 *
 * One fetch of a query evaluated on a worker, waiting to be read on the
 * database's thread.
 */
typedef struct _VentureLqlRead VentureLqlRead;

/**
 * VentureLqlReadDispatch:
 * @read: (transfer full): the fetch to read
 * @user_data: (closure): the data given with the dispatch
 *
 * Called on the worker, from inside a fetch, to hand @read to the
 * database's thread. Whoever receives it calls venture_lql_read_perform()
 * there exactly once, or venture_lql_read_free() to give it up (the
 * executor is then told the query was cancelled). It must not wait for
 * the read: the worker waits for it in its own main context.
 */
typedef void (*VentureLqlReadDispatch)(VentureLqlRead *read, gpointer user_data);

/**
 * venture_lql_set_read_dispatch:
 * @dispatch: (nullable) (scope forever): where the calling thread's fetches go;
 *   %NULL to clear it
 * @user_data: (closure dispatch): data for @dispatch
 *
 * Gives the calling thread -- a query worker -- a way to have its fetches
 * read on the database's thread. Without one, a fetch reads on the
 * calling thread, which must then be the database's.
 */
void
venture_lql_set_read_dispatch(
	VentureLqlReadDispatch	dispatch,
	gpointer		user_data
);

/**
 * venture_lql_read_perform:
 * @read: (transfer full): a fetch handed to a #VentureLqlReadDispatch
 *
 * Reads the fetch on the calling thread, the database's, as the caller
 * the environment was built for (its access snapshot), and gives the
 * rows -- or the error -- to the worker waiting on it. A read whose query
 * was cancelled meanwhile is not read. Consumes @read.
 */
void
venture_lql_read_perform(VentureLqlRead *read);

/**
 * venture_lql_read_get_cancellable:
 * @read: a fetch handed to a #VentureLqlReadDispatch
 *
 * Returns: (transfer none) (nullable): the query's cancellable
 */
GCancellable *
venture_lql_read_get_cancellable(VentureLqlRead *read);

/**
 * venture_lql_read_free:
 * @read: (nullable) (transfer full): a fetch not to be read after all
 *
 * Gives a fetch up unread: the worker's executor is told the query was
 * cancelled, so nothing waits on it for ever.
 */
void
venture_lql_read_free(VentureLqlRead *read);

/**
 * venture_lql_query_error_position:
 * @error: an error from venture_lql_query_run()
 * @out_line: (out) (optional): the 1-based line
 * @out_column: (out) (optional): the 1-based column
 *
 * Where in the query @error is, read from its "LINE:COLUMN: " prefix.
 *
 * Returns: %TRUE when the error has a place
 */
gboolean
venture_lql_query_error_position(
	const GError	*error,
	guint		*out_line,
	guint		*out_column
);

/**
 * venture_lql_query_to_csv:
 * @document: a venture.query/1 document
 *
 * The document's rows as CSV (RFC 4180, CRLF): a header of every column,
 * then one line per row. Text is defused against spreadsheet formulas,
 * money is "1234.56 USD", and a list of money (sum_money()'s one total per
 * currency) is each total joined by "; ", never one figure.
 *
 * Returns: (transfer full): the CSV text
 */
gchar *
venture_lql_query_to_csv(JsonNode *document);

/**
 * venture_web_server_new_lql_environment:
 * @server: the running web server
 * @request: the request a query arrived with
 * @error: (out) (optional): return location for a #GError
 *
 * The one door the web layer's query surfaces use: authenticates
 * @request, admits it as GET /api/v1/<type> admits a reader (hosted
 * lifecycle, viewer role), scopes it to the business selected in the
 * browser and everything beneath it, and gates every record type with the
 * list API's own role check. A query run in the result sees exactly the
 * rows and fields the list API would give the same request.
 *
 * Call it inside the request's own dispatch: the caller's reader is
 * captured from the access scope the server set up for the request.
 *
 * Returns: (transfer full) (nullable): the environment, or %NULL with
 *   @error set (unauthenticated, refused, or no such business)
 */
LqlEnvironment *
venture_web_server_new_lql_environment(
	VentureWebServer	 *server,
	HtmxRequest		 *request,
	GError			**error
);

G_END_DECLS

#endif /* VENTURE_LQL_H */
