/*
 * venture-calendar-view.c - The calendar's sources, the fetch, and its
 * two renderings as data
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The sources live in a registry on the context, not in a list here: the
 * built-in ones are registered by venture_calendar_register_builtin_sources()
 * (venture-calendar-sources.c) and a plugin adds its own the same way. A
 * source is asked only while the record type it names is registered --
 * the module registry's mask is the one switch -- so this file never
 * spells which module owns what.
 */

#include "venture.h"

#include <stdarg.h>
#include <string.h>

#define VENTURE_CALENDAR_SOURCES_KEY "venture-calendar-sources"

/* ==========================================================================
 * Items and results
 * ========================================================================== */

VentureCalendarItem *
venture_calendar_item_new(
	const gchar	*kind,
	const gchar	*title,
	GDateTime	*start,
	const gchar	*record_type,
	gint64		 record_id
){
	VentureCalendarItem *item;

	g_return_val_if_fail(NULL != kind, NULL);
	g_return_val_if_fail(NULL != start, NULL);

	item = g_new0(VentureCalendarItem, 1);
	item->kind = g_strdup(kind);
	item->title = g_strdup(venture_string_is_empty(title) ? "(untitled)" : title);
	item->start = g_date_time_ref(start);
	item->record_type = g_strdup(record_type);
	item->record_id = record_id;

	if ((NULL != record_type) && (record_id > 0))
		item->url = g_strdup_printf("/e/%s/%" G_GINT64_FORMAT, record_type, record_id);

	return item;
}

void
venture_calendar_item_free(VentureCalendarItem *item)
{
	if (NULL == item)
		return;

	g_free(item->kind);
	g_free(item->subkind);
	g_free(item->title);
	g_free(item->detail);
	g_clear_pointer(&item->start, g_date_time_unref);
	g_clear_pointer(&item->end, g_date_time_unref);
	g_free(item->day);
	g_free(item->record_type);
	g_free(item->url);
	g_free(item->status);
	g_free(item->uid);
	g_free(item);
}

/* The day @when falls on in @timezone, a stored calendar date being its
 * own day whatever the zone. */
static gchar *
calendar_day_of(
	GDateTime	*when,
	GTimeZone	*timezone
){
	return venture_time_to_date_string(when, timezone);
}

gboolean
venture_calendar_item_covers_day(
	const VentureCalendarItem	*item,
	const gchar			*day,
	GTimeZone			*timezone
){
	g_autofree gchar *first = NULL;
	g_autofree gchar *last = NULL;

	g_return_val_if_fail(NULL != item, FALSE);
	g_return_val_if_fail(NULL != day, FALSE);

	first = calendar_day_of(item->start, timezone);

	if (g_strcmp0(day, first) < 0)
		return FALSE;

	if (NULL == item->end)
		return 0 == g_strcmp0(day, first);

	/* The end is exclusive: an all-day item ending at midnight on the
	 * 4th is over by the 4th, and a meeting ending at 10:00 is on the
	 * day 10:00 falls on. Step back a microsecond to find the last day
	 * it occupies, never before its first. */
	{
		g_autoptr(GDateTime) before = g_date_time_add(item->end, -1);

		if (g_date_time_compare(before, item->start) < 0)
			last = g_strdup(first);
		else
			last = calendar_day_of(before, timezone);
	}

	return g_strcmp0(day, last) <= 0;
}

void
venture_calendar_query_init(VentureCalendarQuery *query)
{
	g_return_if_fail(NULL != query);

	memset(query, 0, sizeof(*query));
}

VentureCalendarResult *
venture_calendar_result_new(void)
{
	VentureCalendarResult *result;

	result = g_new0(VentureCalendarResult, 1);
	result->items = g_ptr_array_new_with_free_func((GDestroyNotify)venture_calendar_item_free);
	result->notes = g_ptr_array_new_with_free_func(g_free);
	result->kinds = g_ptr_array_new_with_free_func(g_free);

	return result;
}

void
venture_calendar_result_free(VentureCalendarResult *result)
{
	if (NULL == result)
		return;

	g_clear_pointer(&result->items, g_ptr_array_unref);
	g_clear_pointer(&result->notes, g_ptr_array_unref);
	g_clear_pointer(&result->kinds, g_ptr_array_unref);
	g_free(result);
}

void
venture_calendar_result_add(
	VentureCalendarResult	*result,
	VentureCalendarItem	*item
){
	g_return_if_fail(NULL != result);
	g_return_if_fail(NULL != item);

	g_ptr_array_add(result->items, item);
}

void
venture_calendar_result_note(
	VentureCalendarResult	*result,
	const gchar		*format,
	...
){
	va_list args;

	g_return_if_fail(NULL != result);
	g_return_if_fail(NULL != format);

	va_start(args, format);
	g_ptr_array_add(result->notes, g_strdup_vprintf(format, args));
	va_end(args);
}

void
venture_calendar_result_refuse(
	VentureCalendarResult	*result,
	const gchar		*label,
	gint			 bound
){
	g_return_if_fail(NULL != result);

	venture_calendar_result_note(result,
		"%s: more than %d fall in this span, so none are shown; narrow the "
		"span, the venture or the kinds", label, bound);
}

/* ==========================================================================
 * The source registry
 * ========================================================================== */

typedef struct
{
	gchar				*name;
	gchar				*label;
	gchar				*type_name;
	gchar				*module;
	VentureCalendarSourceFlags	 flags;
	VentureCalendarFetchFunc	 fetch;
	gpointer			 user_data;
	GDestroyNotify			 destroy;
} CalendarSource;

static void
calendar_source_free(gpointer data)
{
	CalendarSource *source = data;

	if (NULL == source)
		return;

	if (NULL != source->destroy)
		source->destroy(source->user_data);

	g_free(source->name);
	g_free(source->label);
	g_free(source->type_name);
	g_free(source->module);
	g_free(source);
}

/* The context's sources, made on first use. */
static GPtrArray *
calendar_sources(VentureContext *context)
{
	GPtrArray *sources;

	sources = g_object_get_data(G_OBJECT(context), VENTURE_CALENDAR_SOURCES_KEY);

	if (NULL == sources)
	{
		sources = g_ptr_array_new_with_free_func(calendar_source_free);
		g_object_set_data_full(G_OBJECT(context), VENTURE_CALENDAR_SOURCES_KEY,
		                       sources, (GDestroyNotify)g_ptr_array_unref);
	}

	return sources;
}

static CalendarSource *
calendar_source_lookup(
	VentureContext	*context,
	const gchar	*name,
	guint		*out_index
){
	GPtrArray *sources;
	guint i;

	sources = calendar_sources(context);

	for (i = 0; i < sources->len; i++)
	{
		CalendarSource *source = g_ptr_array_index(sources, i);

		if (0 == g_strcmp0(source->name, name))
		{
			if (NULL != out_index)
				*out_index = i;

			return source;
		}
	}

	return NULL;
}

void
venture_calendar_sources_add(
	VentureContext			*context,
	const gchar			*name,
	const gchar			*label,
	const gchar			*type_name,
	const gchar			*module,
	VentureCalendarSourceFlags	 flags,
	VentureCalendarFetchFunc	 fetch,
	gpointer			 user_data,
	GDestroyNotify			 destroy
){
	CalendarSource *source;
	guint index;

	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_return_if_fail(NULL != name);
	g_return_if_fail(NULL != fetch);

	source = g_new0(CalendarSource, 1);
	source->name = g_strdup(name);
	source->label = g_strdup((NULL != label) ? label : name);
	source->type_name = g_strdup(type_name);
	source->module = g_strdup(module);
	source->flags = flags;
	source->fetch = fetch;
	source->user_data = user_data;
	source->destroy = destroy;

	/* A source with the same name replaces it in place, so the kind
	 * filter keeps its order. */
	if (NULL != calendar_source_lookup(context, name, &index))
	{
		GPtrArray *sources = calendar_sources(context);

		calendar_source_free(g_ptr_array_index(sources, index));
		g_ptr_array_index(sources, index) = source;
		return;
	}

	g_ptr_array_add(calendar_sources(context), source);
}

gboolean
venture_calendar_sources_remove(
	VentureContext	*context,
	const gchar	*name
){
	guint index;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);

	if (NULL == calendar_source_lookup(context, name, &index))
		return FALSE;

	g_ptr_array_remove_index(calendar_sources(context), index);
	return TRUE;
}

/* Whether a source can run now: its type registered (the module mask
 * hides a switched-off module's types) and its module, if it names one,
 * on. */
static gboolean
calendar_source_available(
	VentureContext	*context,
	CalendarSource	*source
){
	if ((NULL != source->type_name) &&
	    (G_TYPE_INVALID == venture_entity_registry_lookup(
		venture_context_get_entity_registry(context), source->type_name)))
		return FALSE;

	if ((NULL != source->module) &&
	    !venture_context_module_enabled(context, source->module))
		return FALSE;

	return TRUE;
}

static GStrv
calendar_source_names(
	VentureContext	*context,
	gboolean	 available_only
){
	g_autoptr(GStrvBuilder) builder = NULL;
	GPtrArray *sources;
	guint i;

	builder = g_strv_builder_new();
	sources = calendar_sources(context);

	for (i = 0; i < sources->len; i++)
	{
		CalendarSource *source = g_ptr_array_index(sources, i);

		if (available_only && !calendar_source_available(context, source))
			continue;

		g_strv_builder_add(builder, source->name);
	}

	return g_strv_builder_end(builder);
}

GStrv
venture_calendar_sources_list(VentureContext *context)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	return calendar_source_names(context, FALSE);
}

GStrv
venture_calendar_sources_available(VentureContext *context)
{
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	return calendar_source_names(context, TRUE);
}

const gchar *
venture_calendar_sources_get_label(
	VentureContext	*context,
	const gchar	*name
){
	CalendarSource *source;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);

	source = calendar_source_lookup(context, name, NULL);

	return (NULL != source) ? source->label : NULL;
}

/* ==========================================================================
 * The span
 * ========================================================================== */

/* A day given as text, as midnight UTC on the day it names in @timezone. */
static GDateTime *
calendar_parse_day(
	const gchar	 *text,
	GTimeZone	 *timezone,
	const gchar	 *name,
	GError		**error
){
	g_autoptr(GDateTime) when = NULL;
	g_autofree gchar *day = NULL;

	when = venture_time_from_string(text, NULL);

	if (NULL == when)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "%s \"%s\" is not a date; write it as YYYY-MM-DD", name, text);
		return NULL;
	}

	day = venture_time_to_date_string(when, timezone);

	return venture_time_from_string(day, error);
}

gboolean
venture_calendar_view_range(
	VentureContext	 *context,
	const gchar	 *view,
	const gchar	 *anchor,
	const gchar	 *from,
	const gchar	 *to,
	GDateTime	**out_from,
	GDateTime	**out_to,
	GError		**error
){
	g_autoptr(GDateTime) day = NULL;
	g_autoptr(GDateTime) first = NULL;
	g_autoptr(GDateTime) end = NULL;
	GTimeZone *timezone;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), FALSE);
	g_return_val_if_fail(NULL != out_from && NULL != out_to, FALSE);

	*out_from = NULL;
	*out_to = NULL;
	timezone = venture_context_get_timezone(context);

	if (venture_string_is_empty(view))
		view = "month";

	if ((0 != g_strcmp0(view, "month")) && (0 != g_strcmp0(view, "week")) &&
	    (0 != g_strcmp0(view, "agenda")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "view \"%s\" is not one of month, week or agenda", view);
		return FALSE;
	}

	if (!venture_string_is_empty(to) && venture_string_is_empty(from))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "to needs a from: name the first day too");
		return FALSE;
	}

	/* --- An explicit span: both days inclusive, as a person writes them --- */

	if (!venture_string_is_empty(from) && !venture_string_is_empty(to))
	{
		g_autoptr(GDateTime) last = NULL;

		first = calendar_parse_day(from, timezone, "from", error);

		if (NULL == first)
			return FALSE;

		last = calendar_parse_day(to, timezone, "to", error);

		if (NULL == last)
			return FALSE;

		if (g_date_time_compare(last, first) < 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "to %s is before from %s", to, from);
			return FALSE;
		}

		end = g_date_time_add_days(last, 1);
	}
	else
	{
		/* --- A view's span around a day --- */

		if (!venture_string_is_empty(from))
			day = calendar_parse_day(from, timezone, "from", error);
		else if (!venture_string_is_empty(anchor))
			day = calendar_parse_day(anchor, timezone, "date", error);
		else
			day = venture_time_today(timezone);

		if (NULL == day)
			return FALSE;

		if (!venture_string_is_empty(from))
		{
			/* A first day and a view: the view's length from that day. */
			first = g_date_time_ref(day);
		}
		else if (0 == g_strcmp0(view, "month"))
		{
			first = g_date_time_add_days(day, 1 - g_date_time_get_day_of_month(day));
		}
		else if (0 == g_strcmp0(view, "week"))
		{
			/* Monday first: ISO weeks, the week the money calendar
			 * and every weekly report use. */
			first = g_date_time_add_days(day, 1 - g_date_time_get_day_of_week(day));
		}
		else
		{
			first = g_date_time_ref(day);
		}

		if (0 == g_strcmp0(view, "month"))
			end = g_date_time_add_months(first, 1);
		else if (0 == g_strcmp0(view, "week"))
			end = g_date_time_add_days(first, 7);
		else
			end = g_date_time_add_days(first, 30);
	}

	if (g_date_time_difference(end, first) > (GTimeSpan)VENTURE_CALENDAR_MAX_DAYS * G_TIME_SPAN_DAY)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "a calendar covers at most %d days; ask for a shorter span",
		            VENTURE_CALENDAR_MAX_DAYS);
		return FALSE;
	}

	*out_from = g_steal_pointer(&first);
	*out_to = g_steal_pointer(&end);

	return TRUE;
}

/* ==========================================================================
 * The fetch
 * ========================================================================== */

/* Whether @item is inside [from, to): it starts before the end, and
 * either it is still running at the start or it starts inside. */
static gboolean
calendar_item_overlaps(
	const VentureCalendarItem	*item,
	GDateTime			*from,
	GDateTime			*to
){
	if (g_date_time_compare(item->start, to) >= 0)
		return FALSE;

	if (g_date_time_compare(item->start, from) >= 0)
		return TRUE;

	return (NULL != item->end) && (g_date_time_compare(item->end, from) > 0);
}

static gint
calendar_item_compare(
	gconstpointer	a,
	gconstpointer	b
){
	const VentureCalendarItem *left = *(VentureCalendarItem *const *)a;
	const VentureCalendarItem *right = *(VentureCalendarItem *const *)b;
	gint order;

	order = g_date_time_compare(left->start, right->start);

	/* All-day items lead their day, as every calendar draws them. */
	if (0 == order)
		order = (left->all_day == right->all_day) ? 0 : (left->all_day ? -1 : 1);

	if (0 == order)
		order = g_strcmp0(left->kind, right->kind);

	if (0 == order)
		order = g_strcmp0(left->title, right->title);

	if (0 == order)
		order = (left->record_id < right->record_id) ? -1 : (left->record_id > right->record_id);

	return order;
}

VentureCalendarResult *
venture_calendar_view_fetch(
	VentureContext			 *context,
	const VentureCalendarQuery	 *query,
	GError				**error
){
	g_autoptr(VentureCalendarResult) result = NULL;
	g_autoptr(GPtrArray) kept = NULL;
	g_autoptr(GDateTime) fetch_from = NULL;
	g_autoptr(GDateTime) fetch_to = NULL;
	VentureCalendarQuery fetch_query;
	GPtrArray *sources;
	GTimeZone *timezone;
	guint i;

	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(NULL != query, NULL);

	if (!venture_context_module_enabled(context, "planner"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "the calendar belongs to the planner module, which is off");
		return NULL;
	}

	/* --- The question --- */

	if ((NULL == query->from) || (NULL == query->to) ||
	    (g_date_time_compare(query->to, query->from) <= 0))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "a calendar needs a span whose end is after its start");
		return NULL;
	}

	if (g_date_time_difference(query->to, query->from) >
	    (GTimeSpan)VENTURE_CALENDAR_MAX_DAYS * G_TIME_SPAN_DAY)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		            "a calendar covers at most %d days; ask for a shorter span",
		            VENTURE_CALENDAR_MAX_DAYS);
		return NULL;
	}

	if ((NULL == query->organization_ids) || (0 == query->n_organizations))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "a calendar needs an organization");
		return NULL;
	}

	/* A kind nobody registered is a typo, told the first time. */
	for (i = 0; (NULL != query->kinds) && (NULL != query->kinds[i]); i++)
	{
		if (NULL == calendar_source_lookup(context, query->kinds[i], NULL))
		{
			g_auto(GStrv) known = venture_calendar_sources_list(context);
			g_autofree gchar *list = g_strjoinv(", ", known);

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			            "kind \"%s\" is not a calendar source; the sources are %s",
			            query->kinds[i], list);
			return NULL;
		}
	}

	/* --- Every source asked for --- */

	result = venture_calendar_result_new();
	sources = calendar_sources(context);
	fetch_query = *query;

	/* Calendar dates stay at midnight UTC. Read the adjacent UTC days so
	 * a local day also finds date-only records outside its timed bounds. */
	if (NULL != query->local_day)
	{
		fetch_from = g_date_time_add_days(query->from, -1);
		fetch_to = g_date_time_add_days(query->to, 1);
		fetch_query.from = fetch_from;
		fetch_query.to = fetch_to;
	}

	for (i = 0; i < sources->len; i++)
	{
		CalendarSource *source = g_ptr_array_index(sources, i);
		gboolean named;

		named = (NULL != query->kinds) &&
		        g_strv_contains((const gchar *const *)query->kinds, source->name);

		if ((NULL != query->kinds) && !named)
			continue;

		/* A switched-off module's source is simply absent; asked for by
		 * name, it says so rather than answer with nothing. */
		if (!calendar_source_available(context, source))
		{
			if (named)
				venture_calendar_result_note(result,
					"%s: not available, because its module is off", source->label);
			continue;
		}

		if ((0 != query->venture_id) &&
		    (0 == (source->flags & VENTURE_CALENDAR_SOURCE_BY_VENTURE)))
		{
			venture_calendar_result_note(result,
				"%s: left out, because they carry no venture", source->label);
			continue;
		}

		if (((0 != query->assignee_user_id) || !venture_string_is_empty(query->assignee_username)) &&
		    (0 == (source->flags & VENTURE_CALENDAR_SOURCE_BY_ASSIGNEE)))
		{
			venture_calendar_result_note(result,
				"%s: left out, because nobody is assigned them", source->label);
			continue;
		}

		if (!source->fetch(context, &fetch_query, result, source->user_data, error))
			return NULL;

		g_ptr_array_add(result->kinds, g_strdup(source->name));
	}

	/* --- Kept to the span, dated in the zone, in order --- */

	timezone = (NULL != query->timezone) ? query->timezone : venture_context_get_timezone(context);
	kept = g_ptr_array_new_with_free_func((GDestroyNotify)venture_calendar_item_free);

	for (i = 0; i < result->items->len; i++)
	{
		VentureCalendarItem *item = g_ptr_array_index(result->items, i);

		if ((NULL != query->local_day) && item->all_day)
		{
			if (!venture_calendar_item_covers_day(item, query->local_day, NULL))
				continue;
		}
		else if (!calendar_item_overlaps(item, query->from, query->to))
			continue;

		g_free(item->day);
		item->day = calendar_day_of(item->start,
			(NULL != query->local_day && item->all_day) ? NULL : timezone);
		g_ptr_array_add(kept, g_steal_pointer(&g_ptr_array_index(result->items, i)));
	}

	/* What was kept was stolen, leaving NULLs the free function skips;
	 * what is left is outside the span. */
	g_ptr_array_unref(result->items);
	result->items = g_steal_pointer(&kept);
	g_ptr_array_sort(result->items, calendar_item_compare);

	return g_steal_pointer(&result);
}

/* ==========================================================================
 * The renderings
 * ========================================================================== */

static void
calendar_json_time(
	JsonBuilder	*builder,
	const gchar	*name,
	GDateTime	*when
){
	json_builder_set_member_name(builder, name);

	if (NULL == when)
	{
		json_builder_add_null_value(builder);
		return;
	}

	{
		g_autofree gchar *text = venture_time_to_string(when);

		json_builder_add_string_value(builder, text);
	}
}

static void
calendar_json_string(
	JsonBuilder	*builder,
	const gchar	*name,
	const gchar	*value
){
	json_builder_set_member_name(builder, name);

	if (NULL == value)
		json_builder_add_null_value(builder);
	else
		json_builder_add_string_value(builder, value);
}

static void
calendar_json_strings(
	JsonBuilder	*builder,
	const gchar	*name,
	GPtrArray	*values
){
	guint i;

	json_builder_set_member_name(builder, name);
	json_builder_begin_array(builder);

	for (i = 0; (NULL != values) && (i < values->len); i++)
		json_builder_add_string_value(builder, g_ptr_array_index(values, i));

	json_builder_end_array(builder);
}

JsonNode *
venture_calendar_result_to_json(
	const VentureCalendarResult	*result,
	const VentureCalendarQuery	*query
){
	g_autoptr(JsonBuilder) builder = NULL;
	guint i;

	g_return_val_if_fail(NULL != result, NULL);
	g_return_val_if_fail(NULL != query, NULL);

	builder = json_builder_new();
	json_builder_begin_object(builder);

	calendar_json_time(builder, "from", query->from);
	calendar_json_time(builder, "to", query->to);

	json_builder_set_member_name(builder, "venture_id");
	json_builder_add_int_value(builder, query->venture_id);
	json_builder_set_member_name(builder, "include_done");
	json_builder_add_boolean_value(builder, query->include_done);
	calendar_json_string(builder, "assignee", query->assignee_username);

	calendar_json_strings(builder, "kinds", result->kinds);
	calendar_json_strings(builder, "notes", result->notes);

	json_builder_set_member_name(builder, "count");
	json_builder_add_int_value(builder, result->items->len);

	json_builder_set_member_name(builder, "items");
	json_builder_begin_array(builder);

	for (i = 0; i < result->items->len; i++)
	{
		VentureCalendarItem *item = g_ptr_array_index(result->items, i);

		json_builder_begin_object(builder);
		calendar_json_string(builder, "kind", item->kind);
		calendar_json_string(builder, "subkind", item->subkind);
		calendar_json_string(builder, "title", item->title);
		calendar_json_string(builder, "detail", item->detail);
		calendar_json_time(builder, "start", item->start);
		calendar_json_time(builder, "end", item->end);
		json_builder_set_member_name(builder, "all_day");
		json_builder_add_boolean_value(builder, item->all_day);
		calendar_json_string(builder, "day", item->day);
		calendar_json_string(builder, "record_type", item->record_type);
		json_builder_set_member_name(builder, "record_id");
		json_builder_add_int_value(builder, item->record_id);
		calendar_json_string(builder, "url", item->url);
		calendar_json_string(builder, "status", item->status);
		json_builder_set_member_name(builder, "overdue");
		json_builder_add_boolean_value(builder, item->overdue);
		calendar_json_string(builder, "uid", item->uid);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

/* What a feed's STATUS says: an event's own word where it has one. */
static const gchar *
calendar_ics_status(const VentureCalendarItem *item)
{
	if (0 == g_strcmp0(item->status, "cancelled"))
		return "CANCELLED";

	if (0 == g_strcmp0(item->status, "tentative"))
		return "TENTATIVE";

	return "CONFIRMED";
}

gchar *
venture_calendar_result_to_ics(const VentureCalendarResult *result)
{
	g_autoptr(GString) calendar = NULL;
	g_autoptr(GDateTime) now = NULL;
	guint i;

	g_return_val_if_fail(NULL != result, NULL);

	calendar = g_string_new("BEGIN:VCALENDAR\r\nVERSION:2.0\r\n"
	                        "PRODID:-//VENTURE//Planner calendar//EN\r\n"
	                        "CALSCALE:GREGORIAN\r\n");
	now = venture_time_now();

	for (i = 0; i < result->items->len; i++)
	{
		VentureCalendarItem *item = g_ptr_array_index(result->items, i);
		g_autoptr(VentureICalEvent) event = venture_ical_event_new();
		g_autoptr(GString) description = g_string_new(NULL);

		event->uid = g_strdup((NULL != item->uid) ? item->uid : "");
		event->summary = g_strdup(item->title);
		event->starts = g_date_time_ref(item->start);
		event->ends = (NULL != item->end) ? g_date_time_ref(item->end) : NULL;
		event->all_day = item->all_day;
		event->status = g_strdup(calendar_ics_status(item));

		if (!venture_string_is_empty(item->detail))
			g_string_append(description, item->detail);

		if (NULL != item->url)
			g_string_append_printf(description, "%s%s",
			                       (0 == description->len) ? "" : "\n", item->url);

		event->description = g_strdup(description->str);

		/* An item with no stable identifier would be a new event on
		 * every refresh; none is written without one. */
		if (venture_string_is_empty(event->uid))
			continue;

		venture_ical_event_append(calendar, event, now);
	}

	g_string_append(calendar, "END:VCALENDAR\r\n");

	return g_string_free(g_steal_pointer(&calendar), FALSE);
}

/* --- org-mode -------------------------------------------------------------- */

/* Kinds that are a due date rather than a time something happens: written
 * as TODO headings with a DEADLINE, which org-agenda warns about ahead. */
static gboolean
calendar_org_is_deadline(const VentureCalendarItem *item)
{
	static const gchar *const deadlines[] = {
		"ticket", "activity", "milestone", "goal", "deal", "money", NULL
	};

	return g_strv_contains(deadlines, item->kind);
}

/* Whether a due date is finished: the statuses every source uses for
 * "over", whichever way it went. */
static gboolean
calendar_org_is_closed(const VentureCalendarItem *item)
{
	static const gchar *const closed[] = {
		"done", "completed", "achieved", "won", "lost", "cancelled",
		"abandoned", "paid", NULL
	};

	return (NULL != item->status) && g_strv_contains(closed, item->status);
}

/* One line of text with no newline in it, so a title or a detail cannot
 * start a heading or a drawer of its own. */
static void
calendar_org_append_line_text(
	GString		*org,
	const gchar	*text
){
	const gchar *p;

	for (p = (NULL != text) ? text : ""; '\0' != *p; p++)
		g_string_append_c(org, (('\n' == *p) || ('\r' == *p)) ? ' ' : *p);
}

/* `<2026-10-16 Fri>` or `<2026-10-16 Fri 10:00>`. The day names are
 * written out rather than taken from the locale: org reads English. */
static void
calendar_org_append_stamp(
	GString		*org,
	GDateTime	*when,
	gboolean	 with_time,
	const gchar	*until_clock
){
	static const gchar *const days[] = {
		"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
	};

	g_string_append_printf(org, "<%04d-%02d-%02d %s",
	                       g_date_time_get_year(when), g_date_time_get_month(when),
	                       g_date_time_get_day_of_month(when),
	                       days[g_date_time_get_day_of_week(when)]);

	if (with_time)
		g_string_append_printf(org, " %02d:%02d", g_date_time_get_hour(when),
		                       g_date_time_get_minute(when));

	if (NULL != until_clock)
		g_string_append_printf(org, "-%s", until_clock);

	g_string_append_c(org, '>');
}

/* The item's time as org writes it: a day, a time, a time range within a
 * day, or a range of days. An all-day item's dates are its stored
 * calendar dates, read in UTC; its end is exclusive, so the range ends
 * the day before. */
static void
calendar_org_append_when(
	GString				*org,
	const VentureCalendarItem	*item,
	GTimeZone			*timezone
){
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;

	if (item->all_day)
	{
		start = g_date_time_to_utc(item->start);

		if (NULL != item->end)
		{
			g_autoptr(GDateTime) utc_end = g_date_time_to_utc(item->end);

			end = g_date_time_add_days(utc_end, -1);
		}

		calendar_org_append_stamp(org, start, FALSE, NULL);

		if ((NULL != end) && (g_date_time_compare(end, start) > 0))
		{
			g_string_append(org, "--");
			calendar_org_append_stamp(org, end, FALSE, NULL);
		}

		return;
	}

	start = g_date_time_to_timezone(item->start, timezone);

	if (NULL != item->end)
		end = g_date_time_to_timezone(item->end, timezone);

	if ((NULL == end) || venture_time_equal(start, end))
	{
		calendar_org_append_stamp(org, start, TRUE, NULL);
		return;
	}

	if ((g_date_time_get_year(start) == g_date_time_get_year(end)) &&
	    (g_date_time_get_day_of_year(start) == g_date_time_get_day_of_year(end)))
	{
		g_autofree gchar *clock = g_date_time_format(end, "%H:%M");

		calendar_org_append_stamp(org, start, TRUE, clock);
		return;
	}

	calendar_org_append_stamp(org, start, TRUE, NULL);
	g_string_append(org, "--");
	calendar_org_append_stamp(org, end, TRUE, NULL);
}

/* A tag org accepts: letters, digits, `_`, `@`; anything else becomes `_`. */
static void
calendar_org_append_tag(
	GString		*org,
	const gchar	*tag
){
	const gchar *p;

	for (p = tag; '\0' != *p; p++)
		g_string_append_c(org, (g_ascii_isalnum(*p) || ('_' == *p) || ('@' == *p)) ? *p : '_');

	g_string_append_c(org, ':');
}

gchar *
venture_calendar_result_to_org(
	const VentureCalendarResult	*result,
	GTimeZone			*timezone,
	const gchar			*base_url,
	const gchar			*from_day,
	const gchar			*to_day
){
	g_autoptr(GString) org = NULL;
	g_autoptr(GTimeZone) zone = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autofree gchar *made = NULL;
	g_autofree gchar *prefix = NULL;
	guint i;

	g_return_val_if_fail(NULL != result, NULL);

	zone = (NULL != timezone) ? g_time_zone_ref(timezone) : g_time_zone_new_utc();
	now = g_date_time_new_now(zone);
	made = g_date_time_format_iso8601(now);

	/* Links are absolute when the install knows its address, so the file
	 * still opens the record from wherever it was saved. */
	if (!venture_string_is_empty(base_url))
		prefix = g_str_has_suffix(base_url, "/") ? g_strndup(base_url, strlen(base_url) - 1)
		                                          : g_strdup(base_url);
	else
		prefix = g_strdup("");

	org = g_string_new("#+title: VENTURE calendar\n");
	g_string_append_printf(org, "#+description: Every dated record from %s to %s\n",
	                       (NULL != from_day) ? from_day : "", (NULL != to_day) ? to_day : "");
	g_string_append_printf(org, "#+created: %s\n#+startup: overview\n", made);

	/* What was left out, where a person reads it. */
	for (i = 0; i < result->notes->len; i++)
	{
		g_string_append(org, "# note: ");
		calendar_org_append_line_text(org, g_ptr_array_index(result->notes, i));
		g_string_append_c(org, '\n');
	}

	g_string_append_c(org, '\n');

	for (i = 0; i < result->items->len; i++)
	{
		VentureCalendarItem *item = g_ptr_array_index(result->items, i);
		gboolean deadline = calendar_org_is_deadline(item);

		/* --- The heading: a state for a due date, then tags --- */

		g_string_append(org, "* ");

		if (deadline)
			g_string_append(org, calendar_org_is_closed(item) ? "DONE " : "TODO ");

		calendar_org_append_line_text(org, venture_string_is_empty(item->title) ? "(untitled)" : item->title);
		g_string_append(org, " :");
		calendar_org_append_tag(org, item->kind);

		if (!venture_string_is_empty(item->subkind) && (0 != g_strcmp0(item->subkind, item->kind)))
			calendar_org_append_tag(org, item->subkind);

		if (item->overdue)
			g_string_append(org, "overdue:");

		g_string_append_c(org, '\n');

		/* --- When: a deadline, or a timestamp the agenda shows --- */

		if (deadline)
			g_string_append(org, "DEADLINE: ");

		calendar_org_append_when(org, item, zone);
		g_string_append_c(org, '\n');

		/* --- The drawer: what ties the entry back to its record --- */

		g_string_append(org, ":PROPERTIES:\n");

		if (!venture_string_is_empty(item->uid))
		{
			g_string_append(org, ":ID: ");
			calendar_org_append_line_text(org, item->uid);
			g_string_append_c(org, '\n');
		}

		g_string_append_printf(org, ":VENTURE_TYPE: %s\n:VENTURE_RECORD: %" G_GINT64_FORMAT "\n",
		                       (NULL != item->record_type) ? item->record_type : "", item->record_id);

		if (!venture_string_is_empty(item->status))
		{
			g_string_append(org, ":VENTURE_STATUS: ");
			calendar_org_append_line_text(org, item->status);
			g_string_append_c(org, '\n');
		}

		if (NULL != item->url)
			g_string_append_printf(org, ":VENTURE_URL: %s%s\n", prefix, item->url);

		g_string_append(org, ":END:\n");

		/* --- The body: the second line and a link --- */

		if (!venture_string_is_empty(item->detail))
		{
			/* A line that starts with a star would be a heading. */
			if ('*' == item->detail[0])
				g_string_append_c(org, ',');

			calendar_org_append_line_text(org, item->detail);
			g_string_append_c(org, '\n');
		}

		if (NULL != item->url)
			g_string_append_printf(org, "[[%s%s][Open in VENTURE]]\n", prefix, item->url);
	}

	return g_string_free(g_steal_pointer(&org), FALSE);
}
