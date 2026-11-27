/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-lql-query.c - One LQL query, run, as the venture.query/1 document
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * Every surface that runs a query -- POST /api/v1/query, venturectl query,
 * the MCP tool venture_query and the dashboard's query widget -- comes
 * through venture_lql_query_run(), so they answer with the same rows: the
 * CLI and the MCP tool read the API's document, and the widget renders it.
 * There is no second way to turn a table into rows to drift from this one.
 *
 * The document (docs/lql.org, "Running a query"):
 *
 *   {"schema": "venture.query/1", "query": TEXT,
 *    "columns": [{"name", "type"}], "rows": [{...}], "row_count": N,
 *    "explain": null | PLAN, "elapsed_ms": N}
 *
 * What a query may not do is decided before it runs and when it fails:
 *
 *   - a `fetch` of a record type the caller's type gate refused gets that
 *     refusal, exactly as GET /api/v1/<type> gives it, rather than LQL's
 *     "no source named"; the refusals were recorded when the environment
 *     was built (venture_lql_environment_get_refusal());
 *   - LQL's errors become VENTURE errors: a query that does not parse,
 *     check or evaluate is invalid_argument (400), a bound
 *     (query.max_rows, query.max_bytes, LQL's own limits) is validation
 *     (422) and the time limit is timeout (504), with a message that
 *     names query.timeout_ms. A filter bar's run is mapped the same way
 *     (venture_lql_query_set_error()).
 */

#include "lql/venture-lql-private.h"

#include <math.h>
#include <string.h>

/* --- Refusals: a record type the gate said no to ------------------------ */

static gboolean
venture_lql_query_check_pipeline(
	LqlEnvironment		 *environment,
	LqlPipeline		 *pipeline,
	GError			**error
);

/*
 * Walks one expression for sub-pipelines (`lookup (fetch company), ...`,
 * `append (fetch deal)`), whose fetches are checked like the main one.
 */
static gboolean
venture_lql_query_check_node(
	LqlEnvironment		 *environment,
	LqlNode			 *node,
	GError			**error
){
	LqlPipeline *pipeline;
	guint i;

	if (NULL == node)
		return TRUE;

	pipeline = lql_node_get_pipeline(node);

	if ((NULL != pipeline) &&
	    !venture_lql_query_check_pipeline(environment, pipeline, error))
		return FALSE;

	for (i = 0; i < lql_node_get_n_children(node); i++)
	{
		if (!venture_lql_query_check_node(environment,
		                                  lql_node_get_child(node, i), error))
			return FALSE;
	}

	return TRUE;
}

/*
 * The name a `fetch` stage reads, written as a bare word (fetch ticket) or
 * a string (fetch "ticket"), the two spellings LQL's checker accepts.
 */
static const gchar *
venture_lql_query_fetch_name(LqlStage *stage)
{
	LqlNode *name;

	if ((0 != g_strcmp0(lql_stage_get_command(stage), "fetch")) ||
	    (0 == lql_stage_get_n_args(stage)))
		return NULL;

	name = lql_stage_get_arg(stage, 0);

	if (LQL_NODE_FIELD == lql_node_get_kind(name))
	{
		const gchar *const *path = lql_node_get_path(name);

		if ((NULL != path) && (NULL != path[0]) && (NULL == path[1]))
			return path[0];

		return NULL;
	}

	if ((LQL_NODE_LITERAL == lql_node_get_kind(name)) &&
	    (LQL_VALUE_TYPE_STRING == lql_value_get_value_type(lql_node_get_value(name))))
		return lql_value_get_string(lql_node_get_value(name));

	return NULL;
}

/*
 * Every fetch in @pipeline and its sub-pipelines: one naming a type the
 * gate refused fails with that refusal.
 */
static gboolean
venture_lql_query_check_pipeline(
	LqlEnvironment		 *environment,
	LqlPipeline		 *pipeline,
	GError			**error
){
	guint i;

	for (i = 0; i < lql_pipeline_get_n_stages(pipeline); i++)
	{
		LqlStage *stage = lql_pipeline_get_stage(pipeline, i);
		g_auto(GStrv) params = NULL;
		const GError *refusal;
		const gchar *name;
		guint j;

		name = venture_lql_query_fetch_name(stage);
		refusal = (NULL != name)
			? venture_lql_environment_get_refusal(environment, name) : NULL;

		if (NULL != refusal)
		{
			g_propagate_error(error, g_error_copy(refusal));
			return FALSE;
		}

		/* A source the query reads: its schema is built now, on the
		 * database's thread (venture_lql_source_prepare()). */
		if (NULL != name)
		{
			LqlSource *source = lql_environment_lookup_source(environment, name);

			if (VENTURE_IS_LQL_SOURCE(source))
				venture_lql_source_prepare(VENTURE_LQL_SOURCE(source));
		}

		for (j = 0; j < lql_stage_get_n_args(stage); j++)
		{
			if (!venture_lql_query_check_node(environment,
			                                  lql_stage_get_arg(stage, j), error))
				return FALSE;
		}

		params = lql_stage_list_params(stage);

		for (j = 0; (NULL != params) && (NULL != params[j]); j++)
		{
			if (!venture_lql_query_check_node(environment,
			                                  lql_stage_get_param(stage, params[j]), error))
				return FALSE;
		}
	}

	return TRUE;
}

/* --- Errors --------------------------------------------------------------- */

/*
 * An LQL error as a VENTURE one, keeping LQL's "LINE:COLUMN: " prefix so
 * venture_lql_query_error_position() can find the place again. Declared
 * in lql/venture-lql-private.h: a filter bar's run is mapped the same way.
 */
void
venture_lql_query_set_error(
	LqlEnvironment		 *environment,
	const GError		 *lql_error,
	GError			**error
){
	if (VENTURE_ERROR == lql_error->domain)
	{
		g_propagate_error(error, g_error_copy(lql_error));
		return;
	}

	/* A fetch given up unread because its query was cancelled comes back
	 * as GIO's cancellation: the same stop as LQL's own. */
	if (g_error_matches(lql_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
		            "the query was cancelled");
		return;
	}

	if (LQL_ERROR != lql_error->domain)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "%s", lql_error->message);
		return;
	}

	switch ((LqlError)lql_error->code)
	{
	case LQL_ERROR_LIMIT:
		/* The time limit is LQL's timeout_ms, set from query.timeout_ms;
		 * every other limit is a bound the query went over. */
		if (NULL != strstr(lql_error->message, "timeout_ms"))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT,
			            "The query ran longer than query.timeout_ms (%" G_GINT64_FORMAT
			            " ms); filter on stored fields right after the fetch, or "
			            "add | limit N",
			            lql_environment_get_limit(environment, LQL_LIMIT_TIMEOUT_MS));
			return;
		}

		/* The memory budget (VENTURE #44) is LQL's max_bytes, set from
		 * query.max_bytes; say which setting it is. */
		if (NULL != strstr(lql_error->message, "(max_bytes)"))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s: over query.max_bytes (%" G_GINT64_FORMAT ")",
			            lql_error->message,
			            lql_environment_get_limit(environment, LQL_LIMIT_MAX_BYTES));
			return;
		}

		if ((NULL == strstr(lql_error->message, "query.max_rows")) &&
		    (NULL != strstr(lql_error->message, "(max_rows)")))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "%s: over query.max_rows (%" G_GINT64_FORMAT ")",
			            lql_error->message,
			            lql_environment_get_limit(environment, LQL_LIMIT_MAX_ROWS));
			return;
		}

		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "%s",
		            lql_error->message);
		return;

	case LQL_ERROR_CANCELLED:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_TIMEOUT, "%s",
		            lql_error->message);
		return;

	case LQL_ERROR_SYNTAX:
	case LQL_ERROR_TYPE:
	case LQL_ERROR_UNKNOWN_FUNCTION:
	case LQL_ERROR_UNKNOWN_COMMAND:
	case LQL_ERROR_UNKNOWN_FIELD:
	case LQL_ERROR_ARGUMENT:
	case LQL_ERROR_SOURCE:
	case LQL_ERROR_EVALUATION:
	default:
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s",
		            lql_error->message);
		return;
	}
}

gboolean
venture_lql_query_error_position(
	const GError	*error,
	guint		*out_line,
	guint		*out_column
){
	const gchar *cursor;
	gchar *end;
	guint64 line;
	guint64 column;

	if ((NULL == error) || (NULL == error->message))
		return FALSE;

	/* "LINE:COLUMN: message", as LQL writes a positioned error. */
	cursor = error->message;

	if (!g_ascii_isdigit(*cursor))
		return FALSE;

	line = g_ascii_strtoull(cursor, &end, 10);

	if ((':' != *end) || !g_ascii_isdigit(end[1]))
		return FALSE;

	column = g_ascii_strtoull(end + 1, &end, 10);

	if ((':' != *end) || (0 == line) || (0 == column) ||
	    (line > G_MAXUINT) || (column > G_MAXUINT))
		return FALSE;

	if (NULL != out_line)
		*out_line = (guint)line;

	if (NULL != out_column)
		*out_column = (guint)column;

	return TRUE;
}

/* --- Values as JSON ------------------------------------------------------- */

/*
 * One LQL value as JSON: NULL is null, a Timestamp its ISO 8601 text, a
 * Duration its LQL literal ("1h30m"), a non-finite Double null, an Array
 * an array and a Record an object with its fields in order. Money is a
 * Record, so it stays {amount, currency, exponent, formatted}.
 */
static JsonNode *
venture_lql_query_value_to_json(LqlValue *value)
{
	JsonNode *node;

	if ((NULL == value) || lql_value_is_null(value))
		return json_node_new(JSON_NODE_NULL);

	switch (lql_value_get_value_type(value))
	{
	case LQL_VALUE_TYPE_BOOLEAN:
		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_boolean(node, lql_value_get_boolean(value));
		return node;

	case LQL_VALUE_TYPE_INT:
		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_int(node, lql_value_get_int(value));
		return node;

	case LQL_VALUE_TYPE_DOUBLE:
		if (!isfinite(lql_value_get_double(value)))
			return json_node_new(JSON_NODE_NULL);

		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_double(node, lql_value_get_double(value));
		return node;

	case LQL_VALUE_TYPE_STRING:
		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_string(node, lql_value_get_string(value));
		return node;

	case LQL_VALUE_TYPE_TIMESTAMP:
	{
		g_autofree gchar *text = NULL;

		text = lql_timestamp_format_iso8601(lql_value_get_timestamp(value));
		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_string(node, text);
		return node;
	}

	case LQL_VALUE_TYPE_DURATION:
	{
		g_autofree gchar *text = NULL;

		text = lql_duration_format(lql_value_get_duration(value));
		node = json_node_new(JSON_NODE_VALUE);
		json_node_set_string(node, text);
		return node;
	}

	case LQL_VALUE_TYPE_ARRAY:
	{
		GPtrArray *items = lql_value_get_array(value);
		g_autoptr(JsonArray) array = json_array_new();
		guint i;

		for (i = 0; (NULL != items) && (i < items->len); i++)
			json_array_add_element(array,
				venture_lql_query_value_to_json(g_ptr_array_index(items, i)));

		node = json_node_new(JSON_NODE_ARRAY);
		json_node_set_array(node, array);
		return node;
	}

	case LQL_VALUE_TYPE_RECORD:
	{
		LqlRecord *record = lql_value_get_record(value);
		g_autoptr(JsonObject) object = json_object_new();
		guint i;

		for (i = 0; (NULL != record) && (i < lql_record_get_n_fields(record)); i++)
			json_object_set_member(object, lql_record_get_field_name(record, i),
				venture_lql_query_value_to_json(lql_record_get_field_value(record, i)));

		node = json_node_new(JSON_NODE_OBJECT);
		json_node_set_object(node, object);
		return node;
	}

	case LQL_VALUE_TYPE_NULL:
	case LQL_VALUE_TYPE_ANY:
	default:
		return json_node_new(JSON_NODE_NULL);
	}
}

/*
 * The document around a table (NULL for an explained query, whose columns
 * and rows are empty).
 */
static JsonNode *
venture_lql_query_document(
	const gchar	*text,
	LqlTable	*table,
	const gchar	*plan,
	gint64		 elapsed_ms
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	guint n_rows;
	guint i;
	guint j;

	n_rows = (NULL != table) ? lql_table_get_n_rows(table) : 0;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "schema");
	json_builder_add_string_value(builder, VENTURE_LQL_QUERY_SCHEMA);
	json_builder_set_member_name(builder, "query");
	json_builder_add_string_value(builder, text);

	json_builder_set_member_name(builder, "columns");
	json_builder_begin_array(builder);

	for (i = 0; (NULL != table) && (i < lql_table_get_n_columns(table)); i++)
	{
		LqlColumn *column = lql_table_get_column(table, i);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "name");
		json_builder_add_string_value(builder, lql_column_get_name(column));
		json_builder_set_member_name(builder, "type");
		json_builder_add_string_value(builder,
			lql_value_type_to_string(lql_column_get_column_type(column)));
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	/* One object per row, members in column order; a column a row lacks
	 * is null rather than missing, so every row has every column. */
	json_builder_set_member_name(builder, "rows");
	json_builder_begin_array(builder);

	for (i = 0; i < n_rows; i++)
	{
		LqlRecord *row = lql_table_get_row(table, i);

		json_builder_begin_object(builder);

		for (j = 0; j < lql_table_get_n_columns(table); j++)
		{
			const gchar *name = lql_column_get_name(lql_table_get_column(table, j));

			json_builder_set_member_name(builder, name);
			json_builder_add_value(builder,
				venture_lql_query_value_to_json(lql_record_get(row, name)));
		}

		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	json_builder_set_member_name(builder, "row_count");
	json_builder_add_int_value(builder, n_rows);
	json_builder_set_member_name(builder, "explain");

	if (NULL != plan)
		json_builder_add_string_value(builder, plan);
	else
		json_builder_add_null_value(builder);

	json_builder_set_member_name(builder, "elapsed_ms");
	json_builder_add_int_value(builder, elapsed_ms);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* --- Running -------------------------------------------------------------- */

/*
 * A query ready to run: checked against the caller's refusals, with the
 * schema of every source it names built. Made on the database's thread;
 * executed on any (docs/lql.org, "Where a query runs").
 */
struct _VentureLqlPrepared
{
	LqlEnvironment	*environment;	/* owned */
	gchar		*text;
	gboolean	 explain;
};

void
venture_lql_prepared_free(VentureLqlPrepared *prepared)
{
	if (NULL == prepared)
		return;

	g_clear_object(&prepared->environment);
	g_clear_pointer(&prepared->text, g_free);
	g_free(prepared);
}

gboolean
venture_lql_text_check_utf8(
	const gchar	 *text,
	GError		**error
){
	if ((NULL == text) || g_utf8_validate(text, -1, NULL))
		return TRUE;

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	                    VENTURE_LQL_NOT_UTF8_MESSAGE);
	return FALSE;
}

VentureLqlPrepared *
venture_lql_query_prepare(
	LqlEnvironment	 *environment,
	const gchar	 *text,
	gboolean	  explain,
	GError		**error
){
	g_autoptr(LqlPipeline) pipeline = NULL;
	g_autoptr(GError) lql_error = NULL;
	g_autofree gchar *trimmed = NULL;
	VentureLqlPrepared *prepared;

	g_return_val_if_fail(LQL_IS_ENVIRONMENT(environment), NULL);

	/* Before anything parses it: LQL's lexer reads UTF-8 alone. The
	 * surfaces refuse it first; this is the floor under all of them. */
	if (!venture_lql_text_check_utf8(text, error))
		return NULL;

	/* Nothing but white space is no query either. */
	trimmed = (NULL != text) ? g_strstrip(g_strdup(text)) : NULL;

	if ((NULL == trimmed) || ('\0' == trimmed[0]))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "A query is required, such as: fetch ticket | limit 10");
		return NULL;
	}

	/* --- refused types first: the list API's own answer ------------- */
	pipeline = lql_parse_with_limits(text,
		lql_environment_get_limit(environment, LQL_LIMIT_MAX_QUERY_CHARS),
		lql_environment_get_limit(environment, LQL_LIMIT_MAX_DEPTH),
		lql_environment_get_limit(environment, LQL_LIMIT_MAX_STAGES), &lql_error);

	if (NULL == pipeline)
	{
		venture_lql_query_set_error(environment, lql_error, error);
		return NULL;
	}

	/* The walk also builds the schema of every source the query names,
	 * here, so the evaluation never reaches the entity registry. */
	if (!venture_lql_query_check_pipeline(environment, pipeline, error))
		return NULL;

	prepared = g_new0(VentureLqlPrepared, 1);
	prepared->environment = g_object_ref(environment);
	prepared->text = g_strdup(text);
	prepared->explain = explain;

	return prepared;
}

JsonNode *
venture_lql_query_execute(
	VentureLqlPrepared	 *prepared,
	GCancellable		 *cancellable,
	GError			**error
){
	g_autoptr(LqlTable) table = NULL;
	g_autoptr(GError) lql_error = NULL;
	g_autofree gchar *plan = NULL;
	gint64 started;

	g_return_val_if_fail(NULL != prepared, NULL);

	started = g_get_monotonic_time();

	if (prepared->explain)
	{
		g_autoptr(LqlQuery) query = NULL;

		query = lql_environment_prepare(prepared->environment, prepared->text, NULL, &lql_error);

		if (NULL == query)
		{
			venture_lql_query_set_error(prepared->environment, lql_error, error);
			return NULL;
		}

		plan = lql_query_explain(query);
	}
	else
	{
		/* Cancelling stops the evaluation at its next step, and a fetch
		 * not yet read is never read (venture_lql_read_perform()). */
		table = lql_environment_run_sync(prepared->environment, prepared->text,
		                                 NULL, cancellable, &lql_error);

		if (NULL == table)
		{
			venture_lql_query_set_error(prepared->environment, lql_error, error);
			return NULL;
		}
	}

	return venture_lql_query_document(prepared->text, table, plan,
		(g_get_monotonic_time() - started) / 1000);
}

JsonNode *
venture_lql_query_run(
	LqlEnvironment	 *environment,
	const gchar	 *text,
	gboolean	  explain,
	GCancellable	 *cancellable,
	GError		**error
){
	g_autoptr(VentureLqlPrepared) prepared = NULL;

	prepared = venture_lql_query_prepare(environment, text, explain, error);

	if (NULL == prepared)
		return NULL;

	return venture_lql_query_execute(prepared, cancellable, error);
}

/* --- CSV ------------------------------------------------------------------ */

/*
 * Money as the canonical round-trippable text ("1234.56 USD"), or NULL
 * when @node is not a money record.
 */
static gchar *
venture_lql_query_money_text(JsonNode *node)
{
	g_autoptr(VentureMoney) money = NULL;
	JsonObject *object;

	if (!JSON_NODE_HOLDS_OBJECT(node))
		return NULL;

	object = json_node_get_object(node);

	if (!json_object_has_member(object, "amount") ||
	    !json_object_has_member(object, "currency") ||
	    !json_object_has_member(object, "exponent"))
		return NULL;

	money = venture_money_from_json(node, NULL, NULL);

	return (NULL != money) ? venture_money_to_string(money) : NULL;
}

/*
 * One CSV cell. Text is defused against spreadsheet formulas
 * (venture_csv_escape()); numbers and booleans are written as JSON writes
 * them; money is "1234.56 USD"; a list of money -- sum_money()'s one total
 * per currency -- is each total, joined by "; ", never added together;
 * anything else structured is compact JSON.
 */
static gchar *
venture_lql_query_csv_cell(JsonNode *node)
{
	g_autofree gchar *money = NULL;

	if ((NULL == node) || JSON_NODE_HOLDS_NULL(node))
		return g_strdup("");

	if (JSON_NODE_HOLDS_VALUE(node))
	{
		if (G_TYPE_STRING == json_node_get_value_type(node))
			return venture_csv_escape(json_node_get_string(node));

		return venture_json_to_string(node, FALSE);
	}

	money = venture_lql_query_money_text(node);

	if (NULL != money)
		return venture_csv_escape(money);

	if (JSON_NODE_HOLDS_ARRAY(node))
	{
		JsonArray *array = json_node_get_array(node);
		g_autoptr(GString) joined = g_string_new(NULL);
		guint i;

		for (i = 0; i < json_array_get_length(array); i++)
		{
			g_autofree gchar *text = NULL;

			text = venture_lql_query_money_text(json_array_get_element(array, i));

			if (NULL == text)
				break;

			g_string_append_printf(joined, "%s%s", (i > 0) ? "; " : "", text);
		}

		if ((json_array_get_length(array) > 0) && (i == json_array_get_length(array)))
			return venture_csv_escape(joined->str);
	}

	{
		g_autofree gchar *json = venture_json_to_string(node, FALSE);

		return venture_csv_escape(json);
	}
}

gchar *
venture_lql_query_to_csv(JsonNode *document)
{
	g_autoptr(GString) csv = NULL;
	JsonObject *root;
	JsonArray *columns;
	JsonArray *rows;
	guint i;
	guint j;

	g_return_val_if_fail(NULL != document, NULL);
	g_return_val_if_fail(JSON_NODE_HOLDS_OBJECT(document), NULL);

	root = json_node_get_object(document);
	columns = json_object_get_array_member(root, "columns");
	rows = json_object_get_array_member(root, "rows");
	csv = g_string_new(NULL);

	/* The header: every column, in order, even when there are no rows. */
	for (i = 0; i < json_array_get_length(columns); i++)
	{
		g_autofree gchar *name = NULL;

		name = venture_csv_escape(json_object_get_string_member(
			json_array_get_object_element(columns, i), "name"));
		g_string_append_printf(csv, "%s%s", (i > 0) ? "," : "", name);
	}

	g_string_append(csv, "\r\n");

	for (j = 0; j < json_array_get_length(rows); j++)
	{
		JsonObject *row = json_array_get_object_element(rows, j);

		for (i = 0; i < json_array_get_length(columns); i++)
		{
			const gchar *name;
			g_autofree gchar *cell = NULL;

			name = json_object_get_string_member(
				json_array_get_object_element(columns, i), "name");
			cell = venture_lql_query_csv_cell(json_object_get_member(row, name));
			g_string_append_printf(csv, "%s%s", (i > 0) ? "," : "", cell);
		}

		g_string_append(csv, "\r\n");
	}

	return g_string_free(g_steal_pointer(&csv), FALSE);
}
