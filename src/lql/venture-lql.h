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

/* The defaults of query.max_rows and query.timeout_ms. */
#define VENTURE_LQL_DEFAULT_MAX_ROWS (10000)
#define VENTURE_LQL_DEFAULT_TIMEOUT_MS (5000)

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
 * functions. Limits come from query.max_rows and query.timeout_ms.
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
 * venture_lql_source_get_entity_type:
 * @self: a #VentureLqlSource
 *
 * Returns: the record type the source reads
 */
GType
venture_lql_source_get_entity_type(VentureLqlSource *self);

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

/* --- Filter bars (venture-lql-filter.c) ----------------------------------- */

/*
 * The most records one filter may select. Its ids go to the database as
 * one IN list, and SQLite and PostgreSQL bound the parameters of one
 * statement (32766 and 65535); this stays clear of both with a count
 * query beside it.
 */
#define VENTURE_LQL_FILTER_MAX_IDS (30000)

/**
 * VentureLqlProblem:
 * @message: what is wrong, as LQL words it, without a position
 * @positioned: whether @offset and @length point into the filter text;
 *   %FALSE for a problem with the query as a whole (the row bound, the
 *   time limit)
 * @offset: byte offset into the filter text of what is wrong
 * @length: its byte length; 0 at the end of the text
 * @name: (nullable): the unknown name it is about ("staus")
 * @suggestions: (nullable) (array zero-terminated=1): what may have been
 *   meant, best first ("status")
 *
 * Why a filter bar's text cannot run, placed in the text the person
 * typed rather than in the query it becomes.
 */
typedef struct
{
	gchar		*message;
	gboolean	 positioned;
	guint		 offset;
	guint		 length;
	gchar		*name;
	GStrv		 suggestions;
} VentureLqlProblem;

void
venture_lql_problem_free(VentureLqlProblem *problem);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureLqlProblem, venture_lql_problem_free)

/**
 * VentureLqlCompletion:
 * @label: what the list shows ("status", "in_progress")
 * @kind: what it is: LQL's kind names ("field", "function", "keyword",
 *   ...) or "value" for a value of the field being compared
 * @detail: (nullable): one line: a type, a signature, a record's name
 * @insert_text: what replaces the range ("\"in_progress\"")
 * @offset: byte offset into the filter text of the text it replaces
 * @length: byte length of the text it replaces
 */
typedef struct
{
	gchar	*label;
	gchar	*kind;
	gchar	*detail;
	gchar	*insert_text;
	guint	 offset;
	guint	 length;
} VentureLqlCompletion;

void
venture_lql_completion_free(VentureLqlCompletion *completion);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureLqlCompletion, venture_lql_completion_free)

/**
 * venture_lql_filter_check:
 * @environment: the caller's environment
 * @type_name: the record type the bar filters ("ticket")
 * @text: (nullable): what the bar holds: a filter expression, or a
 *   `filter` (or `filter_out`) stage
 *
 * Prepares `fetch TYPE | filter TEXT` without reading anything and says
 * what is wrong with it: a syntax error, an unknown function, a `|` (the
 * bar holds one filter, not a pipeline), a `$parameter`, or a field the
 * record does not have -- which LQL would accept and match nothing, so
 * the bar refuses it instead of drawing an empty board.
 *
 * Returns: (transfer full) (nullable): the problem, or %NULL when @text is
 *   blank or prepares
 */
VentureLqlProblem *
venture_lql_filter_check(
	LqlEnvironment	*environment,
	const gchar	*type_name,
	const gchar	*text
);

/**
 * venture_lql_filter_find_ids:
 * @environment: the caller's environment
 * @type_name: the record type
 * @text: the filter text; must not be blank
 * @include_deleted: read soft-deleted records too
 * @cancellable: (nullable): stops the run between stages and rows
 * @out_problem: (out) (transfer full) (optional): why not, on failure
 *
 * Runs `fetch TYPE | filter TEXT` as the caller and gives the ids of the
 * records it keeps, in no particular order. Fails with a problem when the
 * text does not prepare, when the read passes query.max_rows or the time
 * limit, or when it keeps more than %VENTURE_LQL_FILTER_MAX_IDS records.
 *
 * Returns: (transfer full) (nullable) (element-type gint64): the ids, or
 *   %NULL with @out_problem set
 */
GArray *
venture_lql_filter_find_ids(
	LqlEnvironment		 *environment,
	const gchar		 *type_name,
	const gchar		 *text,
	gboolean		  include_deleted,
	GCancellable		 *cancellable,
	VentureLqlProblem	**out_problem
);

/**
 * venture_lql_filter_highlight:
 * @environment: the caller's environment
 * @type_name: the record type
 * @text: the filter text
 * @problem: (nullable): a positioned problem to mark
 *
 * The filter text as HTML: every token in a `<span class="lql-KIND">`
 * (the classes lql_highlight_get_css() styles), escaped, and the range of
 * @problem in a `<mark class="lql-mark">`.
 *
 * Returns: (transfer full): the HTML
 */
gchar *
venture_lql_filter_highlight(
	LqlEnvironment			*environment,
	const gchar			*type_name,
	const gchar			*text,
	const VentureLqlProblem		*problem
);

/**
 * venture_lql_filter_complete:
 * @environment: the caller's environment
 * @type_name: the record type
 * @text: the filter text
 * @offset: the cursor, a byte offset into @text
 *
 * What can be typed at @offset: LQL's own candidates (fields, functions,
 * keywords, constants), and first, where a field is being compared
 * (`status == `, `priority in ["hi`), the values that field takes: the
 * names of an enumeration, or the forge repositories a reference to one
 * may name.
 *
 * Returns: (transfer full) (element-type VentureLqlCompletion): the
 *   candidates, best first
 */
GPtrArray *
venture_lql_filter_complete(
	LqlEnvironment	*environment,
	const gchar	*type_name,
	const gchar	*text,
	guint		 offset
);

/**
 * venture_lql_filter_from_query_string:
 * @entity_type: the record type
 * @params: (element-type utf8 utf8): decoded URL parameters
 *
 * The LQL that means what @params' filters mean to
 * venture_query_apply_query_string(): `status__not_in=done,cancelled` is
 * `status not in ["done", "cancelled"]`. Parameters that are not a filter
 * on a field of @entity_type (paging, order, search, unknown names) are
 * left out, as is a filter with no LQL equivalent (a money amount).
 * Conjuncts are in parameter-name order.
 *
 * Returns: (transfer full) (nullable): the LQL, or %NULL when no parameter
 *   is such a filter
 */
gchar *
venture_lql_filter_from_query_string(
	GType		 entity_type,
	GHashTable	*params
);

/**
 * venture_lql_filter_has_conjunct:
 * @environment: the caller's environment
 * @type_name: the record type
 * @text: (nullable): the filter text
 * @clause: a filter expression
 *
 * Whether @clause is one of the terms @text joins with a top-level
 * `and` (white space aside): whether a quick chip is on.
 *
 * Returns: %TRUE when it is
 */
gboolean
venture_lql_filter_has_conjunct(
	LqlEnvironment	*environment,
	const gchar	*type_name,
	const gchar	*text,
	const gchar	*clause
);

/**
 * venture_lql_filter_toggle:
 * @environment: the caller's environment
 * @type_name: the record type
 * @text: (nullable): the filter text
 * @clause: a filter expression
 *
 * @text without @clause when it is one of its top-level terms, else
 * @text with `and @clause` added (@text parenthesised when it has a
 * top-level `or`): what pressing a quick chip writes into the bar.
 *
 * Returns: (transfer full): the new filter text, "" when nothing is left
 */
gchar *
venture_lql_filter_toggle(
	LqlEnvironment	*environment,
	const gchar	*type_name,
	const gchar	*text,
	const gchar	*clause
);

/**
 * venture_lql_filter_and:
 * @left: (nullable): a filter expression
 * @right: (nullable): another
 *
 * Both, as one filter: `LEFT and RIGHT`, each parenthesised when it has a
 * top-level `or`. A blank side is dropped, and the other is returned as
 * it is, trimmed: the bar shows a person's own filter as they typed it.
 *
 * Returns: (transfer full): the conjunction, "" when both are blank
 */
gchar *
venture_lql_filter_and(
	const gchar	*left,
	const gchar	*right
);

/**
 * VentureLqlChip:
 * @label: what the chip says ("Open", "Mine", "Issued this month")
 * @clause: the term it writes into the bar, or takes out of it
 *   (venture_lql_filter_toggle())
 *
 * A quick filter on a list's bar.
 */
typedef struct
{
	gchar	*label;
	gchar	*clause;
} VentureLqlChip;

void
venture_lql_chip_free(VentureLqlChip *chip);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureLqlChip, venture_lql_chip_free)

/**
 * venture_lql_filter_chips:
 * @entity_type: a #VentureEntity subtype
 *
 * The quick chips a generic list of @entity_type offers, worked out from
 * its field table rather than written per page (docs/lql.org, "Quick
 * chips"):
 *
 *   - Open and Closed, when its lifecycle field (status, stage, state) is
 *     an enumeration with both final and unfinished states;
 *   - Mine, when a field says whose it is: the assigned username
 *     (`assignee == me()`) or the owning user (`owner_user_id == my_id()`);
 *   - this week and this month of its primary date, the first date the
 *     table declares that says when the record happened (else created_at).
 *
 * Every clause is a single top-level term, and every one pushes down to
 * SQL in full.
 *
 * Returns: (transfer full) (element-type VentureLqlChip): the chips, in
 *   the order the bar shows them; empty for a type with none
 */
GPtrArray *
venture_lql_filter_chips(GType entity_type);

G_END_DECLS

#endif /* VENTURE_LQL_H */
