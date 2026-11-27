/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-lql-filter.c - Filter bars: one LQL filter over one record type
 *
 * Copyright (C) 2026 Zach Podbielniak
 *
 * A filter bar holds the part of a query after `fetch TYPE | filter`:
 * `status not in ["done", "cancelled"] and assignee == me()`. Everything
 * here turns that text into the query it means, and maps what LQL says
 * about the query -- spans, diagnostics, completions -- back into the
 * text the person typed, so a caret lands under their typo and not under
 * a prefix they never saw.
 *
 * The query is always
 *
 *   fetch TYPE[, include_deleted: TRUE]
 *   | filter TEXT
 *
 * or, when the text starts with its own `filter` or `filter_out`, the
 * same with `| TEXT`. Nothing the text holds can reach another source or
 * stage: a `|` in it is refused before anything is prepared (see
 * venture_lql_filter_check()), and the environment has no source that
 * reads anything but records through the access policy (docs/lql.org).
 *
 * See docs/lql.org, "Filter bars".
 */

#include "lql/venture-lql-private.h"

#include <string.h>

/* What a bar's text may start with, as its own stage. */
static const gchar * const venture_lql_filter_stage_words[] = {
	"filter_out", "filter"
};

void
venture_lql_problem_free(VentureLqlProblem *problem)
{
	if (NULL == problem)
		return;

	g_free(problem->message);
	g_free(problem->name);
	g_strfreev(problem->suggestions);
	g_free(problem);
}

void
venture_lql_completion_free(VentureLqlCompletion *completion)
{
	if (NULL == completion)
		return;

	g_free(completion->label);
	g_free(completion->kind);
	g_free(completion->detail);
	g_free(completion->insert_text);
	g_free(completion);
}

/* --- The query a bar's text becomes ---------------------------------------- */

/*
 * Whether @text starts (after white space) with a stage word of its own,
 * `filter` or `filter_out`, followed by white space or nothing.
 */
static gboolean
venture_lql_filter_is_stage(const gchar *text)
{
	const gchar *start;
	gsize i;

	start = text;

	while (g_ascii_isspace(*start))
		start++;

	for (i = 0; i < G_N_ELEMENTS(venture_lql_filter_stage_words); i++)
	{
		gsize length;

		length = strlen(venture_lql_filter_stage_words[i]);

		if ((0 == strncmp(start, venture_lql_filter_stage_words[i], length)) &&
		    (('\0' == start[length]) || g_ascii_isspace(start[length])))
			return TRUE;
	}

	return FALSE;
}

/*
 * The query @text means, without a trailing stage, and where @text starts
 * in it. A bar's text is appended unchanged, so an offset into the query
 * at or after *out_prefix is that offset less the prefix in the text.
 */
static gchar *
venture_lql_filter_build(
	const gchar	*type_name,
	const gchar	*text,
	gboolean	 include_deleted,
	guint		*out_prefix
){
	g_autoptr(GString) query = NULL;

	query = g_string_new("fetch ");
	g_string_append(query, type_name);

	if (include_deleted)
		g_string_append(query, ", " VENTURE_LQL_PARAM_INCLUDE_DELETED ": TRUE");

	g_string_append(query, venture_lql_filter_is_stage(text) ? "\n| " : "\n| filter ");
	*out_prefix = (guint)query->len;
	g_string_append(query, text);

	return g_string_free(g_steal_pointer(&query), FALSE);
}

/* Whether @text is NULL or white space alone: no filter at all. */
static gboolean
venture_lql_filter_blank(const gchar *text)
{
	const gchar *p;

	if (NULL == text)
		return TRUE;

	for (p = text; '\0' != *p; p++)
		if (!g_ascii_isspace(*p))
			return FALSE;

	return TRUE;
}

/* @offset into the query, as an offset into the bar's @length bytes. */
static guint
venture_lql_filter_shift(
	guint	offset,
	guint	prefix,
	guint	length
){
	if (offset <= prefix)
		return 0;

	return MIN(offset - prefix, length);
}

/* A problem from an LQL diagnostic over the query, placed in the text. */
static VentureLqlProblem *
venture_lql_problem_from_diagnostic(
	const LqlDiagnostic	*diagnostic,
	guint			 prefix,
	guint			 length
){
	VentureLqlProblem *problem;
	guint end;

	problem = g_new0(VentureLqlProblem, 1);
	problem->message = g_strdup(diagnostic->message);
	problem->positioned = TRUE;
	problem->offset = venture_lql_filter_shift(diagnostic->offset, prefix, length);
	end = venture_lql_filter_shift(diagnostic->offset + diagnostic->length, prefix, length);
	problem->length = (end > problem->offset) ? (end - problem->offset) : 0;
	problem->name = g_strdup(diagnostic->name);
	problem->suggestions = g_strdupv(diagnostic->suggestions);

	/* Preparing can go over a bound too (an operand evaluated while the
	 * query is planned): that is a bound, not a typo. */
	problem->code = (LQL_ERROR_LIMIT == diagnostic->code) ? VENTURE_ERROR_VALIDATION
	                                                      : VENTURE_ERROR_INVALID_ARGUMENT;

	return problem;
}

/* The text of @span in @query, as a new string. */
static gchar *
venture_lql_span_text(
	const gchar		*query,
	const LqlHighlightSpan	*span
){
	return g_strndup(query + span->offset, span->length);
}

/* Whether @span in @query is exactly @word. */
static gboolean
venture_lql_span_is(
	const gchar		*query,
	const LqlHighlightSpan	*span,
	const gchar		*word
){
	return (strlen(word) == span->length) &&
	       (0 == strncmp(query + span->offset, word, span->length));
}

/* --- Checking --------------------------------------------------------------- */

VentureLqlProblem *
venture_lql_filter_check(
	LqlEnvironment	*environment,
	const gchar	*type_name,
	const gchar	*text
){
	g_autofree gchar *query = NULL;
	g_autoptr(GPtrArray) spans = NULL;
	g_autoptr(GPtrArray) diagnostics = NULL;
	g_autoptr(GPtrArray) warnings = NULL;
	g_autoptr(LqlRecord) params = NULL;
	guint prefix;
	guint length;
	guint i;

	g_return_val_if_fail(NULL != environment, NULL);
	g_return_val_if_fail(NULL != type_name, NULL);

	if (venture_lql_filter_blank(text))
		return NULL;

	/* Never handed to LQL's lexer, which reads UTF-8 alone
	 * (venture_lql_text_check_utf8()). A problem of the whole text, with
	 * no place in it: there is no character to put a caret under. */
	if (!g_utf8_validate(text, -1, NULL))
	{
		VentureLqlProblem *problem;

		problem = g_new0(VentureLqlProblem, 1);
		problem->message = g_strdup(VENTURE_LQL_NOT_UTF8_MESSAGE);
		problem->code = VENTURE_ERROR_INVALID_ARGUMENT;

		return problem;
	}

	length = (guint)strlen(text);
	query = venture_lql_filter_build(type_name, text, FALSE, &prefix);

	/*
	 * One filter, not a pipeline: a `|` would start a stage the board
	 * cannot draw (a summarize has no cards), and the bar is a filter.
	 * Found among the spans, so a `|` inside a string is not one.
	 */
	spans = lql_highlight(environment, query);

	for (i = 0; i < spans->len; i++)
	{
		LqlHighlightSpan *span = g_ptr_array_index(spans, i);
		VentureLqlProblem *problem;

		if ((span->offset < prefix) || (LQL_HIGHLIGHT_PIPE != span->kind))
			continue;

		problem = g_new0(VentureLqlProblem, 1);
		problem->message = g_strdup("this holds one filter, so '|' cannot start another stage here");
		problem->code = VENTURE_ERROR_INVALID_ARGUMENT;
		problem->positioned = TRUE;
		problem->offset = span->offset - prefix;
		problem->length = span->length;

		return problem;
	}

	/*
	 * Prepared with an empty parameter record rather than NULL, so a
	 * `$name` is reported here: nothing supplies parameters to a bar.
	 */
	params = lql_record_new();
	diagnostics = lql_assist_diagnose(environment, query, params);

	if (diagnostics->len > 0)
		return venture_lql_problem_from_diagnostic(g_ptr_array_index(diagnostics, 0),
		                                           prefix, length);

	/*
	 * A field the record does not have prepares -- a missing field is
	 * NULL -- and matches nothing. In a bar that is always a typo, and
	 * an empty board is the worst way to say so.
	 */
	warnings = lql_assist_lint(environment, query);

	for (i = 0; i < warnings->len; i++)
	{
		LqlDiagnostic *warning = g_ptr_array_index(warnings, i);

		if ((warning->offset >= prefix) && (LQL_ERROR_UNKNOWN_FIELD == warning->code))
			return venture_lql_problem_from_diagnostic(warning, prefix, length);
	}

	return NULL;
}

/* --- Running ---------------------------------------------------------------- */

GArray *
venture_lql_filter_find_ids(
	LqlEnvironment		 *environment,
	const gchar		 *type_name,
	const gchar		 *text,
	gboolean		  include_deleted,
	GCancellable		 *cancellable,
	VentureLqlProblem	**out_problem
){
	g_autoptr(VentureLqlProblem) problem = NULL;
	g_autoptr(LqlTable) table = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GArray) ids = NULL;
	g_autofree gchar *query = NULL;
	g_autofree gchar *run = NULL;
	guint prefix;
	guint i;

	g_return_val_if_fail(NULL != environment, NULL);
	g_return_val_if_fail(NULL != type_name, NULL);
	g_return_val_if_fail(NULL != text, NULL);

	problem = venture_lql_filter_check(environment, type_name, text);

	if (NULL == problem)
	{
		/* The ids alone: the caller reads the records itself, through
		 * the list's own query, so only the selection is LQL's. On a
		 * line of its own, after anything a comment could swallow. */
		query = venture_lql_filter_build(type_name, text, include_deleted, &prefix);
		run = g_strconcat(query, "\n| fields id", NULL);
		table = lql_environment_run_sync(environment, run, NULL, cancellable, &error);

		if (NULL == table)
		{
			g_autoptr(GError) refusal = NULL;

			/* Worded and classed as POST /api/v1/query words and classes
			 * it: a bound (rows, bytes) is VALIDATION, the time limit or a
			 * cancel TIMEOUT, the rest INVALID_ARGUMENT. */
			venture_lql_query_set_error(environment, error, &refusal);
			problem = g_new0(VentureLqlProblem, 1);
			problem->message = g_strdup(refusal->message);
			problem->code = (VentureError)refusal->code;
		}
		else if (lql_table_get_n_rows(table) > VENTURE_LQL_FILTER_MAX_IDS)
		{
			problem = g_new0(VentureLqlProblem, 1);
			problem->code = VENTURE_ERROR_VALIDATION;
			problem->message = g_strdup_printf(
				"the filter keeps %u records, more than the %d one list can "
				"show; narrow it", lql_table_get_n_rows(table),
				VENTURE_LQL_FILTER_MAX_IDS);
		}
	}

	if (NULL != problem)
	{
		if (NULL != out_problem)
			*out_problem = g_steal_pointer(&problem);

		return NULL;
	}

	ids = g_array_sized_new(FALSE, FALSE, sizeof(gint64), lql_table_get_n_rows(table));

	for (i = 0; i < lql_table_get_n_rows(table); i++)
	{
		LqlValue *id;
		gint64 value;

		id = lql_record_get(lql_table_get_row(table, i), "id");

		if ((NULL == id) || lql_value_is_null(id))
			continue;

		value = lql_value_get_int(id);
		g_array_append_val(ids, value);
	}

	return g_steal_pointer(&ids);
}

/* --- Highlighting ------------------------------------------------------------ */

/* Appends @text[@start, @end) escaped, in a span of @kind's class if any. */
static void
venture_lql_append_segment(
	GString			*html,
	const gchar		*text,
	guint			 start,
	guint			 end,
	const LqlHighlightSpan	*span
){
	g_autofree gchar *piece = NULL;

	if (end <= start)
		return;

	piece = g_strndup(text + start, end - start);

	if (NULL != span)
		g_string_append_printf(html, "<span class=\"lql-%s\">",
		                       lql_highlight_kind_to_string(span->kind));

	venture_html_escape_append(html, piece);

	if (NULL != span)
		g_string_append(html, "</span>");
}

gchar *
venture_lql_filter_highlight(
	LqlEnvironment			*environment,
	const gchar			*type_name,
	const gchar			*text,
	const VentureLqlProblem		*problem
){
	g_autoptr(GString) html = NULL;
	g_autoptr(GPtrArray) spans = NULL;
	g_autoptr(GPtrArray) mine = NULL;
	g_autofree gchar *query = NULL;
	gboolean marking;
	guint mark_start = 0;
	guint mark_end = 0;
	guint prefix;
	guint length;
	guint position;
	guint next;
	guint i;

	g_return_val_if_fail(NULL != type_name, NULL);

	html = g_string_new(NULL);

	if (NULL == text)
		return g_string_free(g_steal_pointer(&html), FALSE);

	/* Not lexed (venture_lql_text_check_utf8()): shown plain, its bad
	 * bytes as U+FFFD, so the page stays UTF-8 too. */
	if (!g_utf8_validate(text, -1, NULL))
	{
		g_autofree gchar *shown = g_utf8_make_valid(text, -1);

		venture_html_escape_append(html, shown);
		return g_string_free(g_steal_pointer(&html), FALSE);
	}

	length = (guint)strlen(text);
	query = venture_lql_filter_build(type_name, text, FALSE, &prefix);
	spans = lql_highlight(environment, query);

	/* The spans of the bar's own text, in its offsets. */
	mine = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < spans->len; i++)
	{
		LqlHighlightSpan *span = g_ptr_array_index(spans, i);
		LqlHighlightSpan *copy;

		if ((span->offset < prefix) || (0 == span->length))
			continue;

		copy = g_new0(LqlHighlightSpan, 1);
		copy->kind = span->kind;
		copy->offset = span->offset - prefix;
		copy->length = MIN(span->length, length - copy->offset);
		g_ptr_array_add(mine, copy);
	}

	marking = (NULL != problem) && problem->positioned;

	if (marking)
	{
		mark_start = MIN(problem->offset, length);
		mark_end = MIN(problem->offset + problem->length, length);
	}

	/*
	 * Walked once, cutting at every span edge and at the mark's: each
	 * piece is either inside one span or between spans, and inside or
	 * outside the mark, so the elements nest.
	 */
	position = 0;
	next = 0;

	while (position < length)
	{
		LqlHighlightSpan *covering = NULL;
		guint end;

		if (marking && (position == mark_start))
			g_string_append(html, (mark_end > mark_start)
				? "<mark class=\"lql-mark\">"
				: "<mark class=\"lql-mark lql-mark-empty\"></mark>");

		while ((next < mine->len) &&
		       (((LqlHighlightSpan *)g_ptr_array_index(mine, next))->offset +
		        ((LqlHighlightSpan *)g_ptr_array_index(mine, next))->length <= position))
			next++;

		end = length;

		if (next < mine->len)
		{
			LqlHighlightSpan *span = g_ptr_array_index(mine, next);

			if (span->offset <= position)
			{
				covering = span;
				end = span->offset + span->length;
			}
			else
				end = span->offset;
		}

		if (marking && (mark_end > mark_start))
		{
			if (position < mark_start)
				end = MIN(end, mark_start);
			else if (position < mark_end)
				end = MIN(end, mark_end);
		}
		else if (marking && (position < mark_start))
			end = MIN(end, mark_start);

		venture_lql_append_segment(html, text, position, end, covering);

		if (marking && (mark_end > mark_start) && (end == mark_end))
			g_string_append(html, "</mark>");

		position = end;
	}

	/* A problem at the very end -- an unfinished query -- is shown as a
	 * place, not as a character. */
	if (marking && (mark_start == length))
		g_string_append(html, "<mark class=\"lql-mark lql-mark-empty\"></mark>");

	return g_string_free(g_steal_pointer(&html), FALSE);
}

/* --- Completion ---------------------------------------------------------- */

/* A new candidate; takes nothing. */
static VentureLqlCompletion *
venture_lql_completion_new(
	const gchar	*label,
	const gchar	*kind,
	const gchar	*detail,
	const gchar	*insert_text,
	guint		 offset,
	guint		 length
){
	VentureLqlCompletion *completion;

	completion = g_new0(VentureLqlCompletion, 1);
	completion->label = g_strdup(label);
	completion->kind = g_strdup(kind);
	completion->detail = g_strdup(detail);
	completion->insert_text = g_strdup(insert_text);
	completion->offset = offset;
	completion->length = length;

	return completion;
}

/* @text as an LQL string literal: quoted, with " and \ escaped. */
static gchar *
venture_lql_quote(const gchar *text)
{
	g_autoptr(GString) out = NULL;
	const gchar *p;

	out = g_string_new("\"");

	for (p = text; '\0' != *p; p++)
	{
		if (('"' == *p) || ('\\' == *p))
			g_string_append_c(out, '\\');

		if ('\n' == *p)
			g_string_append(out, "\\n");
		else
			g_string_append_c(out, *p);
	}

	g_string_append_c(out, '"');

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* Whether @candidate starts with @typed, ignoring ASCII case. */
static gboolean
venture_lql_typed_prefix(
	const gchar	*candidate,
	const gchar	*typed
){
	return (NULL == typed) || ('\0' == *typed) ||
	       (0 == g_ascii_strncasecmp(candidate, typed, strlen(typed)));
}

/*
 * Where the cursor is a value being compared with a field: `status == |`,
 * `status == "in|`, `status == in|` (a bare word, which becomes a string),
 * `priority in ["high", |` and `not in [`. Read off the spans, backwards
 * from the cursor; NULL when it is anything else.
 *
 * Returns: (transfer full) (nullable): the field, with *out_start and
 *   *out_end the query range a value replaces and *out_typed what of it
 *   has been typed
 */
static gchar *
venture_lql_value_context(
	const gchar	 *query,
	GPtrArray	 *spans,
	guint		  prefix,
	guint		  cursor,
	guint		 *out_start,
	guint		 *out_end,
	gchar		**out_typed
){
	LqlHighlightSpan *span;
	gboolean in_list;
	gint anchor;
	gint last;
	gint j;

	/* The last span that starts before the cursor. */
	last = -1;

	for (j = 0; j < (gint)spans->len; j++)
	{
		span = g_ptr_array_index(spans, j);

		if (span->offset >= cursor)
			break;

		if (LQL_HIGHLIGHT_COMMENT != span->kind)
			last = j;
	}

	if (last < 0)
		return NULL;

	span = g_ptr_array_index(spans, last);

	if (span->offset < prefix)
		return NULL;

	if ((('"' == query[span->offset]) &&
	     ((LQL_HIGHLIGHT_STRING == span->kind) || (LQL_HIGHLIGHT_ERROR == span->kind))) &&
	    (cursor <= span->offset + span->length))
	{
		gboolean closed;

		/* Inside a string: an unterminated one runs to the end; a
		 * closed one ends with its quote, and a cursor after that
		 * quote is past the value, not in it. */
		closed = (span->length >= 2) && ('"' == query[span->offset + span->length - 1]) &&
		         ('\\' != query[span->offset + span->length - 2]);

		if (closed && (cursor == span->offset + span->length))
			return NULL;

		*out_start = span->offset;
		*out_end = span->offset + span->length;
		*out_typed = g_strndup(query + span->offset + 1, cursor - span->offset - 1);
		anchor = last - 1;
	}
	else if ((LQL_HIGHLIGHT_FIELD == span->kind) && (cursor == span->offset + span->length))
	{
		/* A bare word being typed where a value goes. */
		*out_start = span->offset;
		*out_end = cursor;
		*out_typed = venture_lql_span_text(query, span);
		anchor = last - 1;
	}
	else if ((cursor > span->offset + span->length) || (LQL_HIGHLIGHT_PUNCTUATION == span->kind) ||
	         (LQL_HIGHLIGHT_OPERATOR == span->kind) || (LQL_HIGHLIGHT_KEYWORD == span->kind))
	{
		/* Nothing typed yet: right after `[`, `,`, `==`, `!=`, `in`. */
		if (!(venture_lql_span_is(query, span, "[") || venture_lql_span_is(query, span, ",") ||
		      venture_lql_span_is(query, span, "==") || venture_lql_span_is(query, span, "!=") ||
		      venture_lql_span_is(query, span, "in")))
			return NULL;

		*out_start = cursor;
		*out_end = cursor;
		*out_typed = g_strdup("");
		anchor = last;
	}
	else
		return NULL;

	/* Back over the list so far: values and commas, then its `[`. */
	in_list = FALSE;
	j = anchor;

	while ((j >= 0) &&
	       ((LQL_HIGHLIGHT_STRING == ((LqlHighlightSpan *)g_ptr_array_index(spans, j))->kind) ||
	        (LQL_HIGHLIGHT_NUMBER == ((LqlHighlightSpan *)g_ptr_array_index(spans, j))->kind) ||
	        venture_lql_span_is(query, g_ptr_array_index(spans, j), ",")))
	{
		if (venture_lql_span_is(query, g_ptr_array_index(spans, j), ","))
			in_list = TRUE;
		j--;
	}

	if ((j >= 0) && venture_lql_span_is(query, g_ptr_array_index(spans, j), "["))
	{
		in_list = TRUE;
		j--;
	}
	else if (in_list || (j != anchor))
		goto none;

	/* The comparison: `in` (or `not in`) before a list, `==` or `!=`
	 * before a single value. */
	if (j < 0)
		goto none;

	span = g_ptr_array_index(spans, j);

	if (in_list ? !venture_lql_span_is(query, span, "in")
	            : !(venture_lql_span_is(query, span, "==") || venture_lql_span_is(query, span, "!=")))
		goto none;

	j--;

	if (in_list && (j >= 0) && venture_lql_span_is(query, g_ptr_array_index(spans, j), "not"))
		j--;

	if ((j < 0) || (LQL_HIGHLIGHT_FIELD != ((LqlHighlightSpan *)g_ptr_array_index(spans, j))->kind) ||
	    (((LqlHighlightSpan *)g_ptr_array_index(spans, j))->offset < prefix))
		goto none;

	return venture_lql_span_text(query, g_ptr_array_index(spans, j));

none:
	g_clear_pointer(out_typed, g_free);
	return NULL;
}

/*
 * The values @field of @type_name takes, matching @typed: an
 * enumeration's names, or for a reference to a forge repository its ids
 * (`repo_id`) or names (`repo_label`), read as the caller through the
 * environment, so a repository they may not read is never offered.
 */
static void
venture_lql_add_values(
	LqlEnvironment	*environment,
	const gchar	*type_name,
	const gchar	*field,
	const gchar	*typed,
	guint		 start,
	guint		 end,
	GCancellable	*cancellable,
	GPtrArray	*out
){
	g_autoptr(VentureEntityClass) klass = NULL;
	g_autofree gchar *property = NULL;
	g_autofree gchar *base = NULL;
	gboolean by_name;
	const gchar *reference;
	GParamSpec *pspec;
	LqlSource *source;
	GType entity_type;

	/* The record type from the environment's own source, not the entity
	 * registry: this runs on a query worker, which touches no registry. */
	source = lql_environment_lookup_source(environment, type_name);

	if (!VENTURE_IS_LQL_SOURCE(source))
		return;

	entity_type = venture_lql_source_get_entity_type(VENTURE_LQL_SOURCE(source));

	klass = g_type_class_ref(entity_type);

	/* `repo_label` names the same reference as `repo_id`. */
	by_name = g_str_has_suffix(field, "_label");

	if (by_name)
	{
		g_autofree gchar *stem = g_strndup(field, strlen(field) - strlen("_label"));

		base = g_strconcat(stem, "_id", NULL);
	}
	else
		base = g_strdup(field);

	property = venture_entity_column_to_property(base);
	pspec = g_object_class_find_property(G_OBJECT_CLASS(klass), property);

	if (NULL == pspec)
		return;

	if (!by_name && G_TYPE_IS_ENUM(pspec->value_type))
	{
		g_autoptr(GEnumClass) values = NULL;
		guint i;

		values = g_type_class_ref(pspec->value_type);

		for (i = 0; i < values->n_values; i++)
		{
			g_autofree gchar *literal = NULL;
			const gchar *nick = values->values[i].value_nick;

			if (!venture_lql_typed_prefix(nick, typed))
				continue;

			literal = venture_lql_quote(nick);
			g_ptr_array_add(out, venture_lql_completion_new(nick, "value",
				g_type_name(pspec->value_type), literal, start, end - start));
		}

		return;
	}

	reference = venture_entity_class_get_reference(klass, property);

	if (0 == g_strcmp0(reference, "forge_repo"))
	{
		g_autoptr(LqlTable) repos = NULL;
		guint i;

		/* At most a page of them, sorted by name; a failure (the
		 * caller may not read repositories) offers none. */
		repos = lql_environment_run_sync(environment,
			"fetch forge_repo | fields id, display_name | sort display_name | limit 100",
			NULL, cancellable, NULL);

		for (i = 0; (NULL != repos) && (i < lql_table_get_n_rows(repos)); i++)
		{
			LqlRecord *row = lql_table_get_row(repos, i);
			LqlValue *name = lql_record_get(row, "display_name");
			g_autofree gchar *id = NULL;
			g_autofree gchar *literal = NULL;
			const gchar *text;

			if ((NULL == name) || lql_value_is_null(name))
				continue;

			text = lql_value_get_string(name);
			id = g_strdup_printf("%" G_GINT64_FORMAT,
			                     lql_value_get_int(lql_record_get(row, "id")));

			if (!venture_lql_typed_prefix(text, typed) && !venture_lql_typed_prefix(id, typed))
				continue;

			if (by_name)
			{
				literal = venture_lql_quote(text);
				g_ptr_array_add(out, venture_lql_completion_new(text, "value", "forge_repo",
					literal, start, end - start));
			}
			else
				g_ptr_array_add(out, venture_lql_completion_new(id, "value", text,
					id, start, end - start));
		}
	}
}

GPtrArray *
venture_lql_filter_complete(
	LqlEnvironment	*environment,
	const gchar	*type_name,
	const gchar	*text,
	guint		 offset,
	GCancellable	*cancellable
){
	g_autoptr(GPtrArray) spans = NULL;
	g_autoptr(GPtrArray) candidates = NULL;
	g_autofree gchar *query = NULL;
	g_autofree gchar *field = NULL;
	g_autofree gchar *typed = NULL;
	GPtrArray *out;
	guint prefix;
	guint length;
	guint start = 0;
	guint end = 0;
	guint i;

	g_return_val_if_fail(NULL != environment, NULL);
	g_return_val_if_fail(NULL != type_name, NULL);

	out = g_ptr_array_new_with_free_func((GDestroyNotify)venture_lql_completion_free);

	if (NULL == text)
		text = "";

	/* Nothing to complete in text LQL may not lex
	 * (venture_lql_text_check_utf8()). */
	if (!g_utf8_validate(text, -1, NULL))
		return out;

	length = (guint)strlen(text);
	offset = MIN(offset, length);
	query = venture_lql_filter_build(type_name, text, FALSE, &prefix);
	spans = lql_highlight(environment, query);

	/* The field's own values first: they are what the comparison
	 * being typed is waiting for. */
	field = venture_lql_value_context(query, spans, prefix, prefix + offset,
	                                  &start, &end, &typed);

	if (NULL != field)
		venture_lql_add_values(environment, type_name, field, typed,
		                       start - prefix, MIN(end - prefix, length), cancellable, out);

	/* Then LQL's. A command or a source cannot appear inside a filter,
	 * and a value inside a string is only ever the field's. */
	candidates = lql_assist_complete(environment, query, prefix + offset);

	for (i = 0; i < candidates->len; i++)
	{
		LqlCompletion *candidate = g_ptr_array_index(candidates, i);

		if ((candidate->offset < prefix) ||
		    (LQL_COMPLETION_COMMAND == candidate->kind) ||
		    (LQL_COMPLETION_SOURCE == candidate->kind) ||
		    (LQL_COMPLETION_PARAMETER_NAME == candidate->kind))
			continue;

		g_ptr_array_add(out, venture_lql_completion_new(candidate->label,
			lql_completion_kind_to_string(candidate->kind), candidate->detail,
			candidate->insert_text, candidate->offset - prefix,
			MIN(candidate->length, length - (candidate->offset - prefix))));
	}

	return out;
}

/* --- URL filters as LQL --------------------------------------------------- */

/* Parameters venture_query_apply_query_string() reads as something other
 * than a filter, and those the pages read for themselves. */
static const gchar * const venture_lql_reserved_params[] = {
	"limit", "offset", "page", "search", "include_deleted", "order",
	"period", "period_field", "q", "view", "list_filters"
};

/* One value of a URL filter on a field of @value_type, as an LQL literal;
 * NULL when it has none. */
static gchar *
venture_lql_literal(
	GType		 value_type,
	const gchar	*text
){
	if ((G_TYPE_STRING == value_type) || G_TYPE_IS_ENUM(value_type))
		return venture_lql_quote(text);

	if ((G_TYPE_INT64 == value_type) || (G_TYPE_INT == value_type) ||
	    (G_TYPE_UINT == value_type) || (G_TYPE_UINT64 == value_type))
	{
		gint64 number;
		gchar *end = NULL;

		number = g_ascii_strtoll(text, &end, 10);

		if ((end == text) || ('\0' != *end))
			return NULL;

		return g_strdup_printf("%" G_GINT64_FORMAT, number);
	}

	if (G_TYPE_DOUBLE == value_type)
	{
		gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];
		gdouble number;
		gchar *end = NULL;

		number = g_ascii_strtod(text, &end);

		if ((end == text) || ('\0' != *end))
			return NULL;

		g_ascii_dtostr(buffer, sizeof(buffer), number);

		/* "2" would read as an Int, which never equals a Double. */
		if (NULL == strpbrk(buffer, ".eEn"))
			return g_strconcat(buffer, ".0", NULL);

		return g_strdup(buffer);
	}

	if (G_TYPE_BOOLEAN == value_type)
		return g_strdup(((0 == g_ascii_strcasecmp(text, "true")) || (0 == g_strcmp0(text, "1")))
		                ? "TRUE" : "FALSE");

	if (G_TYPE_DATE_TIME == value_type)
	{
		g_autofree gchar *quoted = venture_lql_quote(text);

		return g_strdup_printf("timestamp(%s)", quoted);
	}

	return NULL;
}

/* A list of @values as `[a, b]`; NULL when one has no literal. */
static gchar *
venture_lql_literal_list(
	GType		 value_type,
	const gchar	*values
){
	g_autoptr(GString) out = NULL;
	g_auto(GStrv) parts = NULL;
	guint i;

	parts = g_strsplit(values, ",", -1);
	out = g_string_new("[");

	for (i = 0; NULL != parts[i]; i++)
	{
		g_autofree gchar *literal = venture_lql_literal(value_type, g_strstrip(parts[i]));

		if (NULL == literal)
			return NULL;

		if (i > 0)
			g_string_append(out, ", ");

		g_string_append(out, literal);
	}

	g_string_append_c(out, ']');

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * One `field[__op]=value` parameter as an LQL term; NULL when it is not a
 * filter on a field of @klass or has no LQL equivalent.
 */
static gchar *
venture_lql_term(
	VentureEntityClass	*klass,
	const gchar		*name,
	const gchar		*value
){
	g_autofree gchar *field = NULL;
	g_autofree gchar *property = NULL;
	g_autofree gchar *literal = NULL;
	const gchar *separator;
	VentureFilterOp op;
	GParamSpec *pspec;
	gint op_value;

	separator = strstr(name, "__");
	op = VENTURE_FILTER_OP_EQ;

	if (NULL != separator)
	{
		if (!venture_enum_from_nick(VENTURE_TYPE_FILTER_OP, separator + 2, &op_value))
			return NULL;

		op = (VentureFilterOp)op_value;
		field = g_strndup(name, (gsize)(separator - name));
	}
	else
		field = g_strdup(name);

	property = venture_entity_column_to_property(field);
	pspec = g_object_class_find_property(G_OBJECT_CLASS(klass), property);

	/* The column a URL filter reads is the property's, so a field LQL
	 * names differently (a money amount) has no term here. */
	if ((NULL == pspec) || (VENTURE_TYPE_MONEY == pspec->value_type))
		return NULL;

	switch (op)
	{
	case VENTURE_FILTER_OP_IS_NULL:
		return g_strdup_printf("is_null(%s)", field);

	case VENTURE_FILTER_OP_NOT_NULL:
		return g_strdup_printf("is_not_null(%s)", field);

	case VENTURE_FILTER_OP_IN:
	case VENTURE_FILTER_OP_NOT_IN:
		literal = venture_lql_literal_list(pspec->value_type, value);

		if (NULL == literal)
			return NULL;

		return g_strdup_printf("%s %sin %s", field,
		                       (VENTURE_FILTER_OP_NOT_IN == op) ? "not " : "", literal);

	case VENTURE_FILTER_OP_BETWEEN:
	{
		g_auto(GStrv) ends = g_strsplit(value, ",", 2);
		g_autofree gchar *low = NULL;
		g_autofree gchar *high = NULL;

		if ((NULL == ends[0]) || (NULL == ends[1]))
			return NULL;

		low = venture_lql_literal(pspec->value_type, g_strstrip(ends[0]));
		high = venture_lql_literal(pspec->value_type, g_strstrip(ends[1]));

		if ((NULL == low) || (NULL == high))
			return NULL;

		return g_strdup_printf("between(%s, %s, %s)", field, low, high);
	}

	case VENTURE_FILTER_OP_LIKE:
	case VENTURE_FILTER_OP_ILIKE:
		if (G_TYPE_STRING != pspec->value_type)
			return NULL;

		literal = venture_lql_quote(value);

		/* SQL LIKE in VENTURE is case-insensitive for ASCII on SQLite,
		 * and ILIKE everywhere; LQL's like() with ignore_case is the
		 * reading both agree with on what a person meant. */
		return g_strdup_printf("like(%s, %s, TRUE)", field, literal);

	case VENTURE_FILTER_OP_EQ:
	case VENTURE_FILTER_OP_NE:
		/* An empty text value means "null or blank" to a URL filter
		 * (venture-query.c), which LQL says in so many words. */
		if ((G_TYPE_STRING == pspec->value_type) && venture_string_is_empty(value))
			return g_strdup_printf((VENTURE_FILTER_OP_EQ == op)
				? "(is_null(%s) or %s == \"\")"
				: "(is_not_null(%s) and %s != \"\")", field, field);
		G_GNUC_FALLTHROUGH;

	default:
	{
		static const gchar * const symbols[] = { "==", "!=", "<", "<=", ">", ">=" };

		if ((guint)op >= G_N_ELEMENTS(symbols))
			return NULL;

		literal = venture_lql_literal(pspec->value_type, value);

		if (NULL == literal)
			return NULL;

		return g_strdup_printf("%s %s %s", field, symbols[op], literal);
	}
	}
}

/* GCompareFunc over two C strings, for a stable parameter order. */
static gint
venture_lql_compare_names(
	gconstpointer	a,
	gconstpointer	b
){
	return g_strcmp0(*(const gchar * const *)a, *(const gchar * const *)b);
}

gchar *
venture_lql_filter_from_query_string(
	GType		 entity_type,
	GHashTable	*params
){
	g_autoptr(VentureEntityClass) klass = NULL;
	g_autoptr(GPtrArray) names = NULL;
	g_autoptr(GString) out = NULL;
	GHashTableIter iter;
	gpointer key;
	guint i;

	if ((NULL == params) || (0 == g_hash_table_size(params)))
		return NULL;

	klass = g_type_class_ref(entity_type);
	names = g_ptr_array_new();
	g_hash_table_iter_init(&iter, params);

	while (g_hash_table_iter_next(&iter, &key, NULL))
		g_ptr_array_add(names, key);

	g_ptr_array_sort(names, venture_lql_compare_names);
	out = g_string_new(NULL);

	for (i = 0; i < names->len; i++)
	{
		const gchar *name = g_ptr_array_index(names, i);
		g_autofree gchar *term = NULL;
		gboolean reserved = FALSE;
		gsize j;

		for (j = 0; j < G_N_ELEMENTS(venture_lql_reserved_params); j++)
			reserved = reserved || (0 == g_strcmp0(name, venture_lql_reserved_params[j]));

		if (reserved)
			continue;

		term = venture_lql_term(klass, name, g_hash_table_lookup(params, name));

		if (NULL == term)
			continue;

		if (out->len > 0)
			g_string_append(out, " and ");

		g_string_append(out, term);
	}

	if (0 == out->len)
		return NULL;

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- Top-level terms: quick chips ------------------------------------------- */

/*
 * The terms @text joins with `and` outside any bracket, each trimmed, and
 * whether it also has a top-level `or` (then the whole text is one term).
 */
static GPtrArray *
venture_lql_filter_terms(
	LqlEnvironment	*environment,
	const gchar	*text,
	gboolean	*out_has_or
){
	g_autoptr(GPtrArray) spans = NULL;
	g_autofree gchar *query = NULL;
	GPtrArray *terms;
	guint prefix;
	guint start;
	gint depth;
	guint i;

	terms = g_ptr_array_new_with_free_func(g_free);
	*out_has_or = FALSE;

	if (venture_lql_filter_blank(text))
		return terms;

	/* Text LQL may not lex (venture_lql_text_check_utf8()) is one term
	 * as it stands: no chip matches it, and an `and` keeps it whole. */
	if (!g_utf8_validate(text, -1, NULL))
	{
		g_ptr_array_add(terms, g_strstrip(g_strdup(text)));
		*out_has_or = TRUE;
		return terms;
	}

	/* Any type name will do: only the shape of the text is read. */
	query = venture_lql_filter_build("ticket", text, FALSE, &prefix);
	spans = lql_highlight(environment, query);
	start = prefix;
	depth = 0;

	for (i = 0; i < spans->len; i++)
	{
		LqlHighlightSpan *span = g_ptr_array_index(spans, i);

		if (span->offset < prefix)
			continue;

		if (LQL_HIGHLIGHT_PUNCTUATION == span->kind)
		{
			gchar c = query[span->offset];

			if (('(' == c) || ('[' == c) || ('{' == c))
				depth++;
			else if ((')' == c) || (']' == c) || ('}' == c))
				depth--;
		}
		else if ((0 == depth) && (LQL_HIGHLIGHT_KEYWORD == span->kind))
		{
			if (venture_lql_span_is(query, span, "or"))
				*out_has_or = TRUE;
			else if (venture_lql_span_is(query, span, "and"))
			{
				g_ptr_array_add(terms, g_strstrip(g_strndup(query + start, span->offset - start)));
				start = span->offset + span->length;
			}
		}
	}

	g_ptr_array_add(terms, g_strstrip(g_strdup(query + start)));

	if (*out_has_or)
	{
		g_ptr_array_set_size(terms, 0);
		g_ptr_array_add(terms, g_strstrip(g_strdup(text)));
	}

	return terms;
}

/* @text without white space outside strings: how two terms compare. */
static gchar *
venture_lql_squeeze(const gchar *text)
{
	g_autoptr(GString) out = NULL;
	gboolean quoted = FALSE;
	const gchar *p;

	out = g_string_new(NULL);

	for (p = text; '\0' != *p; p++)
	{
		if (quoted && ('\\' == *p) && ('\0' != p[1]))
		{
			g_string_append_c(out, *p++);
			g_string_append_c(out, *p);
			continue;
		}

		if ('"' == *p)
			quoted = !quoted;

		if (!quoted && g_ascii_isspace(*p))
			continue;

		g_string_append_c(out, *p);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

gboolean
venture_lql_filter_has_conjunct(
	LqlEnvironment	*environment,
	const gchar	*type_name,
	const gchar	*text,
	const gchar	*clause
){
	g_autoptr(GPtrArray) terms = NULL;
	g_autofree gchar *wanted = NULL;
	gboolean has_or;
	guint i;

	g_return_val_if_fail(NULL != clause, FALSE);
	(void)type_name;

	terms = venture_lql_filter_terms(environment, text, &has_or);
	wanted = venture_lql_squeeze(clause);

	for (i = 0; i < terms->len; i++)
	{
		g_autofree gchar *term = venture_lql_squeeze(g_ptr_array_index(terms, i));

		if (0 == g_strcmp0(term, wanted))
			return TRUE;
	}

	return FALSE;
}

gchar *
venture_lql_filter_toggle(
	LqlEnvironment	*environment,
	const gchar	*type_name,
	const gchar	*text,
	const gchar	*clause
){
	g_autoptr(GPtrArray) terms = NULL;
	g_autoptr(GString) out = NULL;
	g_autofree gchar *wanted = NULL;
	gboolean removed = FALSE;
	gboolean has_or;
	guint i;

	g_return_val_if_fail(NULL != clause, NULL);
	(void)type_name;

	terms = venture_lql_filter_terms(environment, text, &has_or);
	wanted = venture_lql_squeeze(clause);
	out = g_string_new(NULL);

	for (i = 0; i < terms->len; i++)
	{
		const gchar *term = g_ptr_array_index(terms, i);
		g_autofree gchar *squeezed = venture_lql_squeeze(term);

		if (0 == g_strcmp0(squeezed, wanted))
		{
			removed = TRUE;
			continue;
		}

		if ('\0' == *term)
			continue;

		if (out->len > 0)
			g_string_append(out, " and ");

		/* The whole of an `or`, kept together under the new `and`. */
		if (has_or)
			g_string_append_printf(out, "(%s)", term);
		else
			g_string_append(out, term);
	}

	if (!removed)
	{
		if (out->len > 0)
			g_string_append(out, " and ");

		g_string_append(out, clause);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

gchar *
venture_lql_filter_and(
	const gchar	*left,
	const gchar	*right
){
	g_autoptr(GString) out = NULL;
	const gchar *sides[2];
	guint i;

	sides[0] = left;
	sides[1] = right;
	out = g_string_new(NULL);

	/* One side alone is said as it was typed. */
	if (venture_lql_filter_blank(left) || venture_lql_filter_blank(right))
	{
		g_autofree gchar *only = g_strdup(venture_lql_filter_blank(left) ? right : left);

		return (NULL != only) ? g_strstrip(g_steal_pointer(&only)) : g_strdup("");
	}

	for (i = 0; i < G_N_ELEMENTS(sides); i++)
	{
		g_autoptr(GPtrArray) terms = NULL;
		g_autofree gchar *side = NULL;
		gboolean has_or;

		if (venture_lql_filter_blank(sides[i]))
			continue;

		side = g_strstrip(g_strdup(sides[i]));

		if ('\0' == *side)
			continue;

		terms = venture_lql_filter_terms(NULL, side, &has_or);

		if (out->len > 0)
			g_string_append(out, " and ");

		if (has_or)
			g_string_append_printf(out, "(%s)", side);
		else
			g_string_append(out, side);
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* --- Quick chips, from the field table ------------------------------------------ */

void
venture_lql_chip_free(VentureLqlChip *chip)
{
	if (NULL == chip)
		return;

	g_free(chip->label);
	g_free(chip->clause);
	g_free(chip);
}

/*
 * The states a record's life ends in, whatever its type calls them: a
 * status in this list is "closed", any other "open". Words rather than a
 * table per type, as the status badge's tones are: a new type with a
 * `cancelled` or `paid` state gets the right chips with no code.
 *
 * Kept out on purpose: "sent" (an invoice sent is still owed), "published"
 * and "active" (a live article or subscription is not over) and "posted"
 * (a scheduled entry's only state that has happened): each reads better
 * as open, "not over yet".
 */
static const gchar * const venture_lql_closed_states[] = {
	"done", "cancelled", "canceled", "closed", "closed_won", "closed_lost",
	"completed", "complete", "resolved", "won", "lost", "paid", "void",
	"voided", "rejected", "declined", "expired", "superseded", "converted",
	"accepted", "unqualified", "disqualified", "abandoned", "achieved", "archived",
	"disposed", "written_off", "unsubscribed", "bounced", "complained",
	"exited", "succeeded", "failed", "refused", "interrupted", "rolled_back",
	"yanked", "released", "discarded", "refunded", "reversed", "skipped",
	"skipped_closed", "revoked", "promoted", "invoiced", "executed",
	"applied", "locked", "churned", "finished", "ended", "merged", "retired",
	"withdrawn", "deleted"
};

/*
 * Date fields that are machinery of a schedule or a credential rather
 * than when the record happened: a pause, a lease, an expiry, the last or
 * next run. A list's date chips are never about these.
 */
static gboolean
venture_lql_chip_date_is_schedule(const gchar *name)
{
	return g_str_has_suffix(name, "-until") || g_str_has_suffix(name, "expires-at") ||
	       g_str_has_suffix(name, "-set-at") || g_str_has_prefix(name, "last-") ||
	       g_str_has_prefix(name, "next-") || g_str_has_prefix(name, "remind") ||
	       g_str_has_prefix(name, "lease-") || (0 == g_strcmp0(name, "deleted-at")) ||
	       (0 == g_strcmp0(name, "updated-at"));
}

/* A new chip, both strings taken. */
static VentureLqlChip *
venture_lql_chip_take(
	gchar	*label,
	gchar	*clause
){
	VentureLqlChip *chip = g_new0(VentureLqlChip, 1);

	chip->label = label;
	chip->clause = clause;

	return chip;
}

/* @values as an LQL list of strings: ["done", "cancelled"]. */
static gchar *
venture_lql_chip_list(GPtrArray *values)
{
	g_autoptr(GString) out = NULL;
	guint i;

	out = g_string_new("[");

	for (i = 0; i < values->len; i++)
	{
		g_autofree gchar *quoted = venture_lql_quote(g_ptr_array_index(values, i));

		if (i > 0)
			g_string_append(out, ", ");

		g_string_append(out, quoted);
	}

	g_string_append_c(out, ']');

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * Open and Closed: the type's lifecycle enumeration split by
 * venture_lql_closed_states. Only when the enumeration has both kinds: a
 * type whose every state is final (a reward applied, failed or revoked)
 * or none is (a workspace active or suspended) has no such chips.
 */
static void
venture_lql_chips_status(
	GPtrArray	*chips,
	GObjectClass	*klass,
	GPtrArray	*specs
){
	guint i;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		const gchar *const *choices;
		g_autoptr(GPtrArray) closed = NULL;
		g_autofree gchar *column = NULL;
		g_autofree gchar *list = NULL;
		guint n_open = 0;
		guint j;

		if ((VENTURE_FIELD_ROLE_STATUS != venture_field_spec_get_role(spec)) ||
		    (VENTURE_FIELD_KIND_ENUM != venture_field_spec_get_kind(spec)) ||
		    (NULL == g_object_class_find_property(klass, venture_field_spec_get_name(spec))))
			continue;

		choices = venture_field_spec_get_choices(spec);
		closed = g_ptr_array_new_with_free_func(g_free);

		for (j = 0; (NULL != choices) && (NULL != choices[j]); j++)
		{
			gboolean final = FALSE;
			gsize k;

			for (k = 0; k < G_N_ELEMENTS(venture_lql_closed_states); k++)
				final = final || (0 == g_strcmp0(choices[j], venture_lql_closed_states[k]));

			if (final)
				g_ptr_array_add(closed, g_strdup(choices[j]));
			else
				n_open++;
		}

		/* The first lifecycle field is the record's; a second (a
		 * payment state beside a status) is not what "open" means. */
		if ((0 == closed->len) || (0 == n_open))
			return;

		column = venture_entity_property_to_column(venture_field_spec_get_name(spec));
		list = venture_lql_chip_list(closed);
		g_ptr_array_add(chips, venture_lql_chip_take(g_strdup("Open"),
			g_strdup_printf("%s not in %s", column, list)));
		g_ptr_array_add(chips, venture_lql_chip_take(g_strdup("Closed"),
			g_strdup_printf("%s in %s", column, list)));
		return;
	}
}

/*
 * Mine: the field that says whose the record is. First the one the access
 * policy itself reads as "assigned to" (VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME,
 * a username: `assignee == me()`), then a reference to the owning user
 * (owner_user_id, owner_id: `owner_user_id == my_id()`).
 */
static void
venture_lql_chips_mine(
	GPtrArray	*chips,
	GObjectClass	*klass,
	GPtrArray	*specs
){
	static const gchar * const owners[] = { "owner-user-id", "owner-id" };
	gsize k;
	guint i;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		g_autofree gchar *column = NULL;

		if ((VENTURE_FIELD_KIND_STRING != venture_field_spec_get_kind(spec)) ||
		    (0 == (venture_field_spec_get_flags(spec) & VENTURE_COLUMN_FLAG_ASSIGNED_USERNAME)) ||
		    (NULL == g_object_class_find_property(klass, venture_field_spec_get_name(spec))))
			continue;

		column = venture_entity_property_to_column(venture_field_spec_get_name(spec));
		g_ptr_array_add(chips, venture_lql_chip_take(g_strdup("Mine"),
			g_strdup_printf("%s == me()", column)));
		return;
	}

	for (k = 0; k < G_N_ELEMENTS(owners); k++)
	{
		for (i = 0; i < specs->len; i++)
		{
			VentureFieldSpec *spec = g_ptr_array_index(specs, i);
			g_autofree gchar *column = NULL;

			if ((0 != g_strcmp0(venture_field_spec_get_name(spec), owners[k])) ||
			    (VENTURE_FIELD_KIND_REFERENCE != venture_field_spec_get_kind(spec)) ||
			    (0 != g_strcmp0(venture_field_spec_get_reference_type(spec), "user")) ||
			    (NULL == g_object_class_find_property(klass, owners[k])))
				continue;

			column = venture_entity_property_to_column(owners[k]);
			g_ptr_array_add(chips, venture_lql_chip_take(g_strdup("Mine"),
				g_strdup_printf("%s == my_id()", column)));
			return;
		}
	}
}

/*
 * The date windows: this week and this month of the record's primary
 * date -- the first date field the table declares that says when the
 * record happened (issued, occurred, starts, due), not a schedule's
 * machinery (venture_lql_chip_date_is_schedule()) -- or, with none, when
 * it was created. Each window is one bracketed term, so a chip toggles as
 * one, and both of its ends push down to SQL.
 */
static void
venture_lql_chips_dates(
	GPtrArray	*chips,
	GObjectClass	*klass,
	GPtrArray	*specs
){
	static const struct
	{
		const gchar	*period;
		const gchar	*words;
	} windows[] = {
		{ "this_week",  "this week"  },
		{ "this_month", "this month" }
	};
	g_autofree gchar *column = NULL;
	g_autofree gchar *label = NULL;
	gsize w;
	guint i;

	for (i = 0; (NULL == column) && (i < specs->len); i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		VentureFieldKind kind = venture_field_spec_get_kind(spec);
		const gchar *name = venture_field_spec_get_name(spec);
		GParamSpec *pspec;

		if (((VENTURE_FIELD_KIND_DATE != kind) && (VENTURE_FIELD_KIND_DATETIME != kind)) ||
		    (VENTURE_FIELD_ROLE_TECHNICAL == venture_field_spec_get_role(spec)) ||
		    venture_lql_chip_date_is_schedule(name))
			continue;

		/* A timestamp property, or LQL has no Timestamp to compare. */
		pspec = g_object_class_find_property(klass, name);

		if ((NULL == pspec) || (G_TYPE_DATE_TIME != pspec->value_type))
			continue;

		column = venture_entity_property_to_column(name);
		label = g_strdup(venture_field_spec_get_label(spec));

		/* "Issued at" reads "Issued this week" on a chip. */
		if (g_str_has_suffix(label, " at"))
			label[strlen(label) - 3] = '\0';
	}

	if (NULL == column)
	{
		GParamSpec *pspec = g_object_class_find_property(klass, "created-at");

		if ((NULL == pspec) || (G_TYPE_DATE_TIME != pspec->value_type))
			return;

		column = g_strdup("created_at");
		label = g_strdup("Created");
	}

	for (w = 0; w < G_N_ELEMENTS(windows); w++)
		g_ptr_array_add(chips, venture_lql_chip_take(
			g_strdup_printf("%s %s", label, windows[w].words),
			g_strdup_printf("(%s >= period(\"%s\").start and %s < period(\"%s\").end)",
			                column, windows[w].period, column, windows[w].period)));
}

GPtrArray *
venture_lql_filter_chips(GType entity_type)
{
	g_autoptr(VentureEntity) prototype = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	GObjectClass *klass;
	GPtrArray *chips;

	g_return_val_if_fail(g_type_is_a(entity_type, VENTURE_TYPE_ENTITY), NULL);

	chips = g_ptr_array_new_with_free_func((GDestroyNotify)venture_lql_chip_free);
	prototype = g_object_new(entity_type, NULL);
	specs = venture_entity_get_field_specs(prototype);
	klass = G_OBJECT_GET_CLASS(prototype);

	/* In the order a person reaches for them: where it is, whose, when. */
	venture_lql_chips_status(chips, klass, specs);
	venture_lql_chips_mine(chips, klass, specs);
	venture_lql_chips_dates(chips, klass, specs);

	return chips;
}
