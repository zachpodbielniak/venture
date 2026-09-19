/*
 * venture-money-calendar-event.c - one dated money event
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

struct _VentureMoneyCalendarEvent
{
	GObject		 parent_instance;

	gchar		*kind;
	GDateTime	*date;
	gchar		*day;
	gchar		*week;
	gchar		*anchor_day;
	gchar		*title;
	VentureMoney	*amount;
	gchar		*direction;
	gchar		*record_type;
	gint64		 record_id;
	gchar		*counterparty;
	gint64		 counterparty_id;
	gint64		 venture_id;
	gboolean	 overdue;
	gboolean	 counts_in_net;
	gchar		*slot;
};

enum
{
	PROP_0,
	PROP_KIND,
	PROP_DATE,
	PROP_ANCHOR_DAY,
	PROP_TITLE,
	PROP_AMOUNT,
	PROP_DIRECTION,
	PROP_RECORD_TYPE,
	PROP_RECORD_ID,
	PROP_COUNTERPARTY,
	PROP_COUNTERPARTY_ID,
	PROP_VENTURE_ID,
	PROP_OVERDUE,
	PROP_COUNTS_IN_NET,
	PROP_SLOT,
	N_PROPS
};

static GParamSpec *properties[N_PROPS];

G_DEFINE_FINAL_TYPE(VentureMoneyCalendarEvent, venture_money_calendar_event, G_TYPE_OBJECT)

/* "2026-W03": the ISO week-numbering year, not the calendar year, so the
 * days around New Year land in the week they belong to. */
static gchar *
week_label(GDateTime *date)
{
	return g_strdup_printf("%d-W%02d", g_date_time_get_week_numbering_year(date),
		g_date_time_get_week_of_year(date));
}

static void
set_date(VentureMoneyCalendarEvent *self, GDateTime *date)
{
	g_clear_pointer(&self->date, g_date_time_unref);
	g_clear_pointer(&self->day, g_free);
	g_clear_pointer(&self->week, g_free);
	if (date == NULL)
		return;
	self->date = g_date_time_ref(date);
	self->day = g_date_time_format(date, "%Y-%m-%d");
	self->week = week_label(date);
	if (self->anchor_day == NULL)
		self->anchor_day = g_strdup(self->day);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	VentureMoneyCalendarEvent *self = VENTURE_MONEY_CALENDAR_EVENT(object);
	switch (id)
	{
	case PROP_KIND: g_value_set_string(value, self->kind); break;
	case PROP_DATE: g_value_set_boxed(value, self->date); break;
	case PROP_ANCHOR_DAY: g_value_set_string(value, self->anchor_day); break;
	case PROP_TITLE: g_value_set_string(value, self->title); break;
	case PROP_AMOUNT: g_value_set_boxed(value, self->amount); break;
	case PROP_DIRECTION: g_value_set_string(value, self->direction); break;
	case PROP_RECORD_TYPE: g_value_set_string(value, self->record_type); break;
	case PROP_RECORD_ID: g_value_set_int64(value, self->record_id); break;
	case PROP_COUNTERPARTY: g_value_set_string(value, self->counterparty); break;
	case PROP_COUNTERPARTY_ID: g_value_set_int64(value, self->counterparty_id); break;
	case PROP_VENTURE_ID: g_value_set_int64(value, self->venture_id); break;
	case PROP_OVERDUE: g_value_set_boolean(value, self->overdue); break;
	case PROP_COUNTS_IN_NET: g_value_set_boolean(value, self->counts_in_net); break;
	case PROP_SLOT: g_value_set_string(value, self->slot); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	VentureMoneyCalendarEvent *self = VENTURE_MONEY_CALENDAR_EVENT(object);
	switch (id)
	{
	case PROP_KIND: g_free(self->kind); self->kind = g_value_dup_string(value); break;
	case PROP_DATE: set_date(self, g_value_get_boxed(value)); break;
	case PROP_ANCHOR_DAY: g_free(self->anchor_day); self->anchor_day = g_value_dup_string(value); break;
	case PROP_TITLE: g_free(self->title); self->title = g_value_dup_string(value); break;
	case PROP_AMOUNT: g_clear_pointer(&self->amount, venture_money_free); self->amount = g_value_dup_boxed(value); break;
	case PROP_DIRECTION: g_free(self->direction); self->direction = g_value_dup_string(value); break;
	case PROP_RECORD_TYPE: g_free(self->record_type); self->record_type = g_value_dup_string(value); break;
	case PROP_RECORD_ID: self->record_id = g_value_get_int64(value); break;
	case PROP_COUNTERPARTY: g_free(self->counterparty); self->counterparty = g_value_dup_string(value); break;
	case PROP_COUNTERPARTY_ID: self->counterparty_id = g_value_get_int64(value); break;
	case PROP_VENTURE_ID: self->venture_id = g_value_get_int64(value); break;
	case PROP_OVERDUE: self->overdue = g_value_get_boolean(value); break;
	case PROP_COUNTS_IN_NET: self->counts_in_net = g_value_get_boolean(value); break;
	case PROP_SLOT: g_free(self->slot); self->slot = g_value_dup_string(value); break;
	default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
	}
}

static void
finalize(GObject *object)
{
	VentureMoneyCalendarEvent *self = VENTURE_MONEY_CALENDAR_EVENT(object);
	g_free(self->kind);
	g_clear_pointer(&self->date, g_date_time_unref);
	g_free(self->day);
	g_free(self->week);
	g_free(self->anchor_day);
	g_free(self->title);
	g_clear_pointer(&self->amount, venture_money_free);
	g_free(self->direction);
	g_free(self->record_type);
	g_free(self->counterparty);
	g_free(self->slot);
	G_OBJECT_CLASS(venture_money_calendar_event_parent_class)->finalize(object);
}

static void
venture_money_calendar_event_class_init(VentureMoneyCalendarEventClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	GParamFlags rw = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	object_class->finalize = finalize;
	properties[PROP_KIND] = g_param_spec_string("kind", "Kind", "Source kind", NULL, rw);
	properties[PROP_DATE] = g_param_spec_boxed("date", "Date", "Calendar day", G_TYPE_DATE_TIME, rw);
	properties[PROP_ANCHOR_DAY] = g_param_spec_string("anchor-day", "Anchor day", "Original day before any carry", NULL, rw);
	properties[PROP_TITLE] = g_param_spec_string("title", "Title", NULL, NULL, rw);
	properties[PROP_AMOUNT] = g_param_spec_boxed("amount", "Amount", NULL, VENTURE_TYPE_MONEY, rw);
	properties[PROP_DIRECTION] = g_param_spec_string("direction", "Direction", "in or out", NULL, rw);
	properties[PROP_RECORD_TYPE] = g_param_spec_string("record-type", "Record type", NULL, NULL, rw);
	properties[PROP_RECORD_ID] = g_param_spec_int64("record-id", "Record id", NULL, 0, G_MAXINT64, 0, rw);
	properties[PROP_COUNTERPARTY] = g_param_spec_string("counterparty", "Counterparty", "Vendor or customer name", NULL, rw);
	properties[PROP_COUNTERPARTY_ID] = g_param_spec_int64("counterparty-id", "Counterparty id", NULL, 0, G_MAXINT64, 0, rw);
	properties[PROP_VENTURE_ID] = g_param_spec_int64("venture-id", "Venture id", NULL, 0, G_MAXINT64, 0, rw);
	properties[PROP_OVERDUE] = g_param_spec_boolean("overdue", "Overdue", "Carried to today", FALSE, rw);
	properties[PROP_COUNTS_IN_NET] = g_param_spec_boolean("counts-in-net", "Counts in net", "Cash that moves, not a reminder", TRUE, rw);
	properties[PROP_SLOT] = g_param_spec_string("slot", "Slot", "Distinguishes several events of one record on one day", NULL, rw);
	g_object_class_install_properties(object_class, N_PROPS, properties);
}

static void
venture_money_calendar_event_init(VentureMoneyCalendarEvent *self)
{
	self->counts_in_net = TRUE;
}

VentureMoneyCalendarEvent *
venture_money_calendar_event_new(const gchar *kind, GDateTime *date, const gchar *title,
	const VentureMoney *amount, const gchar *direction, const gchar *record_type, gint64 record_id)
{
	g_return_val_if_fail(kind != NULL && date != NULL && amount != NULL, NULL);
	g_return_val_if_fail(g_strcmp0(direction, "in") == 0 || g_strcmp0(direction, "out") == 0, NULL);
	return g_object_new(VENTURE_TYPE_MONEY_CALENDAR_EVENT, "kind", kind, "date", date, "title", title,
		"amount", amount, "direction", direction, "record-type", record_type, "record-id", record_id, NULL);
}

const gchar *venture_money_calendar_event_get_kind(VentureMoneyCalendarEvent *self) { return self->kind; }
GDateTime *venture_money_calendar_event_get_date(VentureMoneyCalendarEvent *self) { return self->date; }
const gchar *venture_money_calendar_event_get_day(VentureMoneyCalendarEvent *self) { return self->day; }
const gchar *venture_money_calendar_event_get_week(VentureMoneyCalendarEvent *self) { return self->week; }
const gchar *venture_money_calendar_event_get_anchor_day(VentureMoneyCalendarEvent *self) { return self->anchor_day; }
const gchar *venture_money_calendar_event_get_title(VentureMoneyCalendarEvent *self) { return self->title; }
const VentureMoney *venture_money_calendar_event_get_amount(VentureMoneyCalendarEvent *self) { return self->amount; }
const gchar *venture_money_calendar_event_get_direction(VentureMoneyCalendarEvent *self) { return self->direction; }
const gchar *venture_money_calendar_event_get_record_type(VentureMoneyCalendarEvent *self) { return self->record_type; }
gint64 venture_money_calendar_event_get_record_id(VentureMoneyCalendarEvent *self) { return self->record_id; }
const gchar *venture_money_calendar_event_get_counterparty(VentureMoneyCalendarEvent *self) { return self->counterparty; }
gint64 venture_money_calendar_event_get_counterparty_id(VentureMoneyCalendarEvent *self) { return self->counterparty_id; }
gint64 venture_money_calendar_event_get_venture_id(VentureMoneyCalendarEvent *self) { return self->venture_id; }
gboolean venture_money_calendar_event_get_overdue(VentureMoneyCalendarEvent *self) { return self->overdue; }
gboolean venture_money_calendar_event_get_counts_in_net(VentureMoneyCalendarEvent *self) { return self->counts_in_net; }
const gchar *venture_money_calendar_event_get_slot(VentureMoneyCalendarEvent *self) { return self->slot; }

const gchar *
venture_money_calendar_event_get_currency(VentureMoneyCalendarEvent *self)
{
	return self->amount ? venture_money_get_currency(self->amount) : NULL;
}

/* The anchor day, not the displayed day, so a bill carried from the 20th to
 * today keeps one identity in a subscribed phone calendar. */
gchar *
venture_money_calendar_event_get_uid(VentureMoneyCalendarEvent *self)
{
	return g_strdup_printf("money-%s-%s-%" G_GINT64_FORMAT "-%s%s%s@venture", self->kind,
		self->record_type ? self->record_type : "none", self->record_id, self->anchor_day,
		self->slot ? "-" : "", self->slot ? self->slot : "");
}

VentureMoney *
venture_money_calendar_event_signed_amount(VentureMoneyCalendarEvent *self)
{
	if (g_strcmp0(self->direction, "out") == 0)
		return venture_money_negate(self->amount);
	return venture_money_copy(self->amount);
}
