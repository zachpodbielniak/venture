/*
 * venture-money-calendar-expander.c - sources, the expander, nets and the feed
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"
#include "money-calendar/venture-money-calendar-private.h"

#include <string.h>

/* --- The source interface -------------------------------------------------- */

G_DEFINE_INTERFACE(VentureMoneyCalendarSource, venture_money_calendar_source, G_TYPE_OBJECT)

static const gchar *
default_module(VentureMoneyCalendarSource *self)
{
	(void)self;
	return NULL;
}

static void
venture_money_calendar_source_default_init(VentureMoneyCalendarSourceInterface *iface)
{
	iface->get_module = default_module;
}

const gchar *
venture_money_calendar_source_get_kind(VentureMoneyCalendarSource *self)
{
	g_return_val_if_fail(VENTURE_IS_MONEY_CALENDAR_SOURCE(self), NULL);
	return VENTURE_MONEY_CALENDAR_SOURCE_GET_IFACE(self)->get_kind(self);
}

const gchar *
venture_money_calendar_source_get_module(VentureMoneyCalendarSource *self)
{
	g_return_val_if_fail(VENTURE_IS_MONEY_CALENDAR_SOURCE(self), NULL);
	return VENTURE_MONEY_CALENDAR_SOURCE_GET_IFACE(self)->get_module(self);
}

gboolean
venture_money_calendar_source_expand(VentureMoneyCalendarSource *self, VentureContext *context,
	gint64 organization_id, const VentureDateRange *range, GDateTime *today, GPtrArray *events, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_MONEY_CALENDAR_SOURCE(self), FALSE);
	return VENTURE_MONEY_CALENDAR_SOURCE_GET_IFACE(self)->expand(self, context, organization_id,
		range, today, events, error);
}

/* --- The expander ---------------------------------------------------------- */

struct _VentureMoneyCalendarExpander
{
	GObject		 parent_instance;
	GPtrArray	*sources;
};

G_DEFINE_FINAL_TYPE(VentureMoneyCalendarExpander, venture_money_calendar_expander, G_TYPE_OBJECT)

static void
expander_finalize(GObject *object)
{
	VentureMoneyCalendarExpander *self = VENTURE_MONEY_CALENDAR_EXPANDER(object);
	g_clear_pointer(&self->sources, g_ptr_array_unref);
	G_OBJECT_CLASS(venture_money_calendar_expander_parent_class)->finalize(object);
}

static void
venture_money_calendar_expander_class_init(VentureMoneyCalendarExpanderClass *klass)
{
	G_OBJECT_CLASS(klass)->finalize = expander_finalize;
}

static void
venture_money_calendar_expander_init(VentureMoneyCalendarExpander *self)
{
	self->sources = g_ptr_array_new_with_free_func(g_object_unref);
}

VentureMoneyCalendarExpander *
venture_money_calendar_expander_new(void)
{
	return g_object_new(VENTURE_TYPE_MONEY_CALENDAR_EXPANDER, NULL);
}

void
venture_money_calendar_expander_add(VentureMoneyCalendarExpander *self, VentureMoneyCalendarSource *source)
{
	const gchar *kind;
	guint i;
	g_return_if_fail(VENTURE_IS_MONEY_CALENDAR_EXPANDER(self));
	g_return_if_fail(VENTURE_IS_MONEY_CALENDAR_SOURCE(source));
	kind = venture_money_calendar_source_get_kind(source);
	for (i = 0; i < self->sources->len; i++)
	{
		if (g_strcmp0(venture_money_calendar_source_get_kind(g_ptr_array_index(self->sources, i)), kind) == 0)
		{
			g_ptr_array_remove_index(self->sources, i);
			break;
		}
	}
	g_ptr_array_add(self->sources, g_object_ref(source));
}

GPtrArray *
venture_money_calendar_expander_kinds(VentureMoneyCalendarExpander *self)
{
	GPtrArray *kinds = g_ptr_array_new();
	guint i;
	g_return_val_if_fail(VENTURE_IS_MONEY_CALENDAR_EXPANDER(self), kinds);
	for (i = 0; i < self->sources->len; i++)
		g_ptr_array_add(kinds, (gpointer)venture_money_calendar_source_get_kind(g_ptr_array_index(self->sources, i)));
	return kinds;
}

static gint
compare_events(gconstpointer a, gconstpointer b)
{
	VentureMoneyCalendarEvent *left = *(VentureMoneyCalendarEvent *const *)a;
	VentureMoneyCalendarEvent *right = *(VentureMoneyCalendarEvent *const *)b;
	gint order = g_strcmp0(venture_money_calendar_event_get_day(left), venture_money_calendar_event_get_day(right));
	if (order == 0)
		order = g_strcmp0(venture_money_calendar_event_get_kind(left), venture_money_calendar_event_get_kind(right));
	if (order == 0)
		order = g_strcmp0(venture_money_calendar_event_get_title(left), venture_money_calendar_event_get_title(right));
	return order;
}

GPtrArray *
venture_money_calendar_expander_expand(VentureMoneyCalendarExpander *self, VentureContext *context,
	gint64 organization_id, const VentureDateRange *range, GDateTime *today, GError **error)
{
	g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;
	g_return_val_if_fail(VENTURE_IS_MONEY_CALENDAR_EXPANDER(self), NULL);
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(range != NULL && today != NULL, NULL);
	for (i = 0; i < self->sources->len; i++)
	{
		VentureMoneyCalendarSource *source = g_ptr_array_index(self->sources, i);
		const gchar *module = venture_money_calendar_source_get_module(source);
		if (module != NULL && !venture_context_module_enabled(context, module))
			continue;
		if (!venture_money_calendar_source_expand(source, context, organization_id, range, today, events, error))
			return NULL;
	}
	g_ptr_array_sort(events, compare_events);
	return g_steal_pointer(&events);
}

VentureMoneyCalendarExpander *
venture_money_calendar_expander_get_default(void)
{
	static VentureMoneyCalendarExpander *expander = NULL;
	if (g_once_init_enter_pointer(&expander))
	{
		VentureMoneyCalendarExpander *fresh = venture_money_calendar_expander_new();
		venture_money_calendar_add_builtin_sources(fresh);
		g_once_init_leave_pointer(&expander, fresh);
	}
	return expander;
}

/* --- Convenience ------------------------------------------------------------ */

GPtrArray *
venture_money_calendar_events(VentureContext *context, gint64 organization_id,
	const VentureDateRange *range, GDateTime *today, GError **error)
{
	g_autoptr(GDateTime) now = NULL;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), NULL);
	g_return_val_if_fail(range != NULL, NULL);
	if (!venture_context_module_enabled(context, "money_calendar"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"The money_calendar module is off");
		return NULL;
	}
	if (today == NULL)
		today = now = venture_time_now();
	return venture_money_calendar_expander_expand(venture_money_calendar_expander_get_default(),
		context, organization_id, range, today, error);
}

GPtrArray *
venture_money_calendar_filter(GPtrArray *events, const gchar *kind, gint64 company_id,
	gint64 venture_id, const gchar *currency)
{
	GPtrArray *kept = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;
	g_return_val_if_fail(events != NULL, kept);
	for (i = 0; i < events->len; i++)
	{
		VentureMoneyCalendarEvent *event = g_ptr_array_index(events, i);
		if (!venture_string_is_empty(kind) && g_strcmp0(venture_money_calendar_event_get_kind(event), kind) != 0)
			continue;
		if (company_id > 0 && venture_money_calendar_event_get_counterparty_id(event) != company_id)
			continue;
		if (venture_id > 0 && venture_money_calendar_event_get_venture_id(event) != venture_id)
			continue;
		if (!venture_string_is_empty(currency) && g_strcmp0(venture_money_calendar_event_get_currency(event), currency) != 0)
			continue;
		g_ptr_array_add(kept, g_object_ref(event));
	}
	return kept;
}

/* --- Nets ---------------------------------------------------------------------- */

/* Keyed "bucket|CUR". Two currencies in one day are two entries; the table
 * never holds a sum that crossed a currency. */
GHashTable *
venture_money_calendar_nets(GPtrArray *events, gboolean by_week)
{
	GHashTable *nets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	guint i;
	g_return_val_if_fail(events != NULL, nets);
	for (i = 0; i < events->len; i++)
	{
		VentureMoneyCalendarEvent *event = g_ptr_array_index(events, i);
		g_autoptr(VentureMoney) signed_amount = NULL;
		g_autofree gchar *key = NULL;
		VentureMoney *running;
		if (!venture_money_calendar_event_get_counts_in_net(event))
			continue;
		signed_amount = venture_money_calendar_event_signed_amount(event);
		key = g_strdup_printf("%s|%s", by_week ? venture_money_calendar_event_get_week(event)
			: venture_money_calendar_event_get_day(event), venture_money_get_currency(signed_amount));
		running = g_hash_table_lookup(nets, key);
		if (running == NULL)
		{
			g_hash_table_insert(nets, g_steal_pointer(&key), g_steal_pointer(&signed_amount));
			continue;
		}
		{
			VentureMoney *sum = venture_money_add(running, signed_amount, NULL);
			if (sum != NULL)
				g_hash_table_insert(nets, g_strdup(key), sum);
		}
	}
	return nets;
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
	return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

gchar *
venture_money_calendar_nets_format(GHashTable *nets, const gchar *bucket)
{
	g_autoptr(GPtrArray) keys = g_ptr_array_new();
	g_autoptr(GString) text = g_string_new("");
	g_autofree gchar *prefix = NULL;
	GHashTableIter iter;
	gpointer key;
	guint i;
	g_return_val_if_fail(nets != NULL && bucket != NULL, g_strdup(""));
	prefix = g_strdup_printf("%s|", bucket);
	g_hash_table_iter_init(&iter, nets);
	while (g_hash_table_iter_next(&iter, &key, NULL))
		if (g_str_has_prefix(key, prefix))
			g_ptr_array_add(keys, key);
	g_ptr_array_sort(keys, compare_strings);
	for (i = 0; i < keys->len; i++)
	{
		g_autofree gchar *amount = venture_money_to_string(g_hash_table_lookup(nets, g_ptr_array_index(keys, i)));
		if (text->len > 0)
			g_string_append_c(text, ' ');
		g_string_append(text, amount);
	}
	return g_string_free(g_steal_pointer(&text), FALSE);
}

/* --- The feed ------------------------------------------------------------------- */

/* All-day events: DTSTART;VALUE=DATE and an exclusive DTEND the next day, as
 * RFC 5545 asks. The line writer is the activities module's, so folding and
 * escaping are the same bytes a phone already accepts from /api/v1/activities.ics. */
gchar *
venture_money_calendar_ics(GPtrArray *events)
{
	g_autoptr(GString) calendar = g_string_new("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//VENTURE//Money calendar//EN\r\nCALSCALE:GREGORIAN\r\n");
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) now_utc = g_date_time_to_utc(now);
	g_autofree gchar *stamp = g_date_time_format(now_utc, "%Y%m%dT%H%M%SZ");
	guint i;
	g_return_val_if_fail(events != NULL, NULL);
	for (i = 0; i < events->len; i++)
	{
		VentureMoneyCalendarEvent *event = g_ptr_array_index(events, i);
		GDateTime *date = venture_money_calendar_event_get_date(event);
		g_autoptr(GDateTime) next = g_date_time_add_days(date, 1);
		g_autofree gchar *start = g_date_time_format(date, "%Y%m%d");
		g_autofree gchar *end = g_date_time_format(next, "%Y%m%d");
		g_autofree gchar *uid = venture_money_calendar_event_get_uid(event);
		g_autofree gchar *amount = venture_money_to_string(venture_money_calendar_event_get_amount(event));
		g_autofree gchar *description = g_strdup_printf("%s %s %s%s\n%s/%" G_GINT64_FORMAT,
			venture_money_calendar_event_get_direction(event), amount,
			venture_money_calendar_event_get_kind(event),
			venture_money_calendar_event_get_overdue(event) ? " (overdue)" : "",
			venture_money_calendar_event_get_record_type(event), venture_money_calendar_event_get_record_id(event));
		g_string_append(calendar, "BEGIN:VEVENT\r\n");
		venture_activity_calendar_append_line(calendar, "UID", uid, FALSE);
		venture_activity_calendar_append_line(calendar, "DTSTAMP", stamp, FALSE);
		venture_activity_calendar_append_line(calendar, "DTSTART;VALUE=DATE", start, FALSE);
		venture_activity_calendar_append_line(calendar, "DTEND;VALUE=DATE", end, FALSE);
		venture_activity_calendar_append_line(calendar, "SUMMARY", venture_money_calendar_event_get_title(event), TRUE);
		venture_activity_calendar_append_line(calendar, "DESCRIPTION", description, TRUE);
		venture_activity_calendar_append_line(calendar, "CATEGORIES", venture_money_calendar_event_get_kind(event), TRUE);
		venture_activity_calendar_append_line(calendar, "STATUS", "CONFIRMED", FALSE);
		g_string_append(calendar, "END:VEVENT\r\n");
	}
	g_string_append(calendar, "END:VCALENDAR\r\n");
	return g_string_free(g_steal_pointer(&calendar), FALSE);
}
