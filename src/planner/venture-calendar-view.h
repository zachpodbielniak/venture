/*
 * venture-calendar-view.h - One calendar over every dated record
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The planner's unified calendar. Every module that dates something --
 * events, tickets, activities, milestones, goals, sprints, deals, the
 * money calendar, imported CalDAV events -- contributes through a source
 * registered on the context, so a later module or a plugin adds a source
 * without touching the page, the API, the feed or the CLI, all of which
 * read the same #VentureCalendarResult.
 *
 * A source names the record type (and optionally the module) it needs and
 * is skipped while that type is hidden. Every source reads through
 * #VentureQuery, bounded by the date range, the organization scope and
 * venture_aggregate_get_max_rows(); past the bound the source is refused
 * with a note and contributes nothing -- a calendar that silently showed
 * the first N would look complete and not be.
 */

#ifndef VENTURE_CALENDAR_VIEW_H
#define VENTURE_CALENDAR_VIEW_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * VENTURE_CALENDAR_MAX_DAYS:
 *
 * The longest span one calendar may cover. A recurring daily event is
 * expanded once per day of the span, so the span is what bounds the
 * expansion; a year and a day covers "this year" and any month view.
 */
#define VENTURE_CALENDAR_MAX_DAYS (367)

/**
 * VENTURE_CALENDAR_MAX_OCCURRENCES:
 *
 * The most occurrences one recurring event may contribute to one
 * calendar. A daily event over the longest span stays under it; it is the
 * backstop against a range that was not bounded.
 */
#define VENTURE_CALENDAR_MAX_OCCURRENCES (400)

/**
 * VentureCalendarItem:
 * @kind: the source that produced it -- event, ticket, activity,
 *   milestone, goal, sprint, deal, money or caldav -- which is also what
 *   the kind filter names
 * @subkind: (nullable): the record's own kind: an event's or activity's
 *   kind, a money item's source
 * @title: what a person reads on the grid
 * @detail: (nullable): a second line: where, who, or what it relates to
 * @start: when it begins (UTC); a calendar date for an all-day item
 * @end: (nullable): when it is over, exclusive; %NULL for a moment
 * @all_day: whether it takes whole days, so its times are dates
 * @day: the calendar day it starts on in the configured zone, YYYY-MM-DD
 * @record_type: the record it came from
 * @record_id: that record's id
 * @url: the page to open, site-relative
 * @status: the record's status nick, for colouring and the feed
 * @overdue: due before now and still open
 * @uid: a stable identifier for the calendar feed
 *
 * One dated thing on the calendar.
 */
typedef struct
{
	gchar		*kind;
	gchar		*subkind;
	gchar		*title;
	gchar		*detail;
	GDateTime	*start;
	GDateTime	*end;
	gboolean	 all_day;
	gchar		*day;
	gchar		*record_type;
	gint64		 record_id;
	gchar		*url;
	gchar		*status;
	gboolean	 overdue;
	gchar		*uid;
} VentureCalendarItem;

/**
 * venture_calendar_item_new:
 * @kind: the source name
 * @title: the title
 * @start: (transfer none): the start
 * @record_type: the record type
 * @record_id: the record id
 *
 * Makes an item whose url is `/e/TYPE/ID`; a source fills the rest.
 *
 * Returns: (transfer full): a new item
 */
VentureCalendarItem *
venture_calendar_item_new(
	const gchar	*kind,
	const gchar	*title,
	GDateTime	*start,
	const gchar	*record_type,
	gint64		 record_id
);

/**
 * venture_calendar_item_free:
 * @item: (nullable): an item
 */
void venture_calendar_item_free(VentureCalendarItem *item);

/**
 * venture_calendar_item_covers_day:
 * @item: an item
 * @day: a calendar day, YYYY-MM-DD
 * @timezone: (nullable): the configured zone; %NULL means UTC
 *
 * Whether @item is on @day: an item spanning several days -- a trip, a
 * sprint -- is on each of them. The month grid asks this rather than
 * working spans out itself.
 *
 * Returns: %TRUE if @item falls on @day
 */
gboolean
venture_calendar_item_covers_day(
	const VentureCalendarItem	*item,
	const gchar			*day,
	GTimeZone			*timezone
);

/**
 * VentureCalendarQuery:
 * @from: the first instant, inclusive
 * @to: the end, exclusive
 * @kinds: (nullable) (array zero-terminated=1): the sources asked for;
 *   %NULL for every available one
 * @venture_id: narrow to one venture, or 0
 * @assignee_user_id: narrow to what this user owns, or 0
 * @assignee_username: (nullable): the same user's name, which tickets and
 *   activities store as their assignee and owner
 * @include_done: also show finished items (done tickets, completed
 *   milestones, achieved goals, won deals, cancelled events)
 * @organization_ids: (array length=n_organizations): the organizations to
 *   read, primary first
 * @n_organizations: how many; must be at least one
 * @now: (nullable): the instant overdue is judged against; %NULL is now
 *
 * What a calendar is asked for. Filled field by field after
 * venture_calendar_query_init(), which zeroes it.
 */
typedef struct
{
	GDateTime	*from;
	GDateTime	*to;
	gchar		**kinds;
	gint64		 venture_id;
	gint64		 assignee_user_id;
	const gchar	*assignee_username;
	gboolean	 include_done;
	const gint64	*organization_ids;
	gsize		 n_organizations;
	GDateTime	*now;
} VentureCalendarQuery;

/**
 * venture_calendar_query_init:
 * @query: (out caller-allocates): the query to clear
 */
void venture_calendar_query_init(VentureCalendarQuery *query);

/**
 * VentureCalendarResult:
 * @items: (element-type VentureCalendarItem): every item, by start, kind
 *   and title
 * @notes: (element-type utf8): what was left out and why, in words
 * @kinds: (element-type utf8): the sources that were asked and ran
 *
 * What a calendar answered. A note is never optional reading: a source
 * refused past its bound, or left out because it cannot be narrowed the
 * way the query asked, says so here.
 */
typedef struct
{
	GPtrArray	*items;
	GPtrArray	*notes;
	GPtrArray	*kinds;
} VentureCalendarResult;

/**
 * venture_calendar_result_new:
 *
 * Returns: (transfer full): an empty result
 */
VentureCalendarResult *venture_calendar_result_new(void);

/**
 * venture_calendar_result_free:
 * @result: (nullable): a result
 */
void venture_calendar_result_free(VentureCalendarResult *result);

/**
 * venture_calendar_result_add:
 * @result: a result
 * @item: (transfer full): an item to keep
 */
void venture_calendar_result_add(VentureCalendarResult *result, VentureCalendarItem *item);

/**
 * venture_calendar_result_note:
 * @result: a result
 * @format: a printf format
 * @...: its arguments
 *
 * Adds a note a person will read beside the calendar.
 */
void venture_calendar_result_note(VentureCalendarResult *result, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureCalendarItem, venture_calendar_item_free)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureCalendarResult, venture_calendar_result_free)

/**
 * VentureCalendarSourceFlags:
 * @VENTURE_CALENDAR_SOURCE_NONE: answers neither narrowing
 * @VENTURE_CALENDAR_SOURCE_BY_VENTURE: its records carry a venture, so it
 *   can answer a venture filter
 * @VENTURE_CALENDAR_SOURCE_BY_ASSIGNEE: its records carry an owner, so it
 *   can answer an assignee filter
 *
 * What a source can be narrowed by. A source asked to narrow by something
 * its records do not carry is left out with a note rather than shown
 * whole: a venture's calendar that included every unrelated activity
 * would be answering a different question.
 */
typedef enum
{
	VENTURE_CALENDAR_SOURCE_NONE = 0,
	VENTURE_CALENDAR_SOURCE_BY_VENTURE = 1 << 0,
	VENTURE_CALENDAR_SOURCE_BY_ASSIGNEE = 1 << 1
} VentureCalendarSourceFlags;

/**
 * VentureCalendarFetchFunc:
 * @context: the context
 * @query: what was asked
 * @result: where to add items and notes
 * @user_data: the data given at registration
 * @error: (out) (optional): the error
 *
 * Adds a source's items for @query to @result. Past the bound a source
 * adds a note through venture_calendar_result_refuse() and no items, and
 * returns %TRUE: one source refused is not the calendar failing.
 *
 * Returns: %FALSE with @error set when the source could not be read
 */
typedef gboolean (*VentureCalendarFetchFunc)(
	VentureContext			 *context,
	const VentureCalendarQuery	 *query,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
);

/**
 * venture_calendar_sources_add:
 * @context: the context whose calendar gains the source
 * @name: the kind its items carry, `[a-z0-9_]+`; a source with the same
 *   name replaces it
 * @label: what the kind filter calls it
 * @type_name: (nullable): the record type it reads; while that type is
 *   hidden the source is skipped
 * @module: (nullable): a module that must also be on
 * @flags: what it can be narrowed by
 * @fetch: (scope notified): the function that reads it
 * @user_data: (closure): data for @fetch
 * @destroy: (destroy user_data) (nullable): frees @user_data
 *
 * Registers a calendar source. The built-in ones are registered when the
 * planner module installs; a plugin adds its own the same way.
 */
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
);

/**
 * venture_calendar_sources_remove:
 * @context: the context
 * @name: a source name
 *
 * Returns: %TRUE if a source was removed
 */
gboolean venture_calendar_sources_remove(VentureContext *context, const gchar *name);

/**
 * venture_calendar_sources_list:
 * @context: the context
 *
 * Returns: (transfer full): every registered source's name, in
 *   registration order, available or not
 */
GStrv venture_calendar_sources_list(VentureContext *context);

/**
 * venture_calendar_sources_available:
 * @context: the context
 *
 * Returns: (transfer full): the names of the sources whose type and
 *   module are on now, in registration order
 */
GStrv venture_calendar_sources_available(VentureContext *context);

/**
 * venture_calendar_sources_get_label:
 * @context: the context
 * @name: a source name
 *
 * Returns: (transfer none) (nullable): the source's label
 */
const gchar *venture_calendar_sources_get_label(VentureContext *context, const gchar *name);

/**
 * venture_calendar_view_range:
 * @context: the context, for its timezone
 * @view: (nullable): month, week or agenda; %NULL is month
 * @anchor: (nullable): a day in the span, YYYY-MM-DD; %NULL is today
 * @from: (nullable): an explicit first day, overriding the view's span
 * @to: (nullable): an explicit last day, inclusive
 * @out_from: (out) (transfer full): the first instant
 * @out_to: (out) (transfer full): the end, exclusive
 * @error: (out) (optional): the error
 *
 * The span a view covers: a month from its first day, a week from its
 * Monday, an agenda for thirty days from the anchor. Every boundary is a
 * midnight UTC, the calendar-date rule; the zone only decides which day
 * "today" is. An explicit span longer than %VENTURE_CALENDAR_MAX_DAYS, or
 * ending before it begins, is refused.
 *
 * Returns: %TRUE on success
 */
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
);

/**
 * venture_calendar_view_fetch:
 * @context: the context
 * @query: what is asked
 * @error: (out) (optional): the error
 *
 * Runs every available source that @query asks for. Refused with
 * %VENTURE_ERROR_VALIDATION while the planner module is off, for a kind
 * nobody registered, and for a span past %VENTURE_CALENDAR_MAX_DAYS.
 *
 * Returns: (transfer full) (nullable): the result
 */
VentureCalendarResult *
venture_calendar_view_fetch(
	VentureContext			 *context,
	const VentureCalendarQuery	 *query,
	GError				**error
);

/**
 * venture_calendar_result_refuse:
 * @result: a result
 * @label: the source's label
 * @bound: the bound it passed
 *
 * The note a source adds instead of its items when it found more than
 * @bound rows: it says what was left out and how to narrow it.
 */
void venture_calendar_result_refuse(VentureCalendarResult *result, const gchar *label, gint bound);

/**
 * venture_calendar_result_to_json:
 * @result: a result
 * @query: the query that produced it
 *
 * The one rendering of a calendar as data: the API answers it and the
 * page is drawn from the same items.
 *
 * Returns: (transfer full): a JSON object
 */
JsonNode *venture_calendar_result_to_json(const VentureCalendarResult *result, const VentureCalendarQuery *query);

/**
 * venture_calendar_result_to_ics:
 * @result: a result
 *
 * The calendar as an RFC 5545 VCALENDAR, one VEVENT per item, written
 * with the same folding and escaping as the CalDAV sync
 * (venture_ical_event_append()).
 *
 * Returns: (transfer full): the text
 */
gchar *venture_calendar_result_to_ics(const VentureCalendarResult *result);

/**
 * venture_calendar_result_to_org:
 * @result: a result
 * @timezone: (nullable): the zone times are written in; %NULL is UTC
 * @base_url: (nullable): the install's public address; links are
 *   site-relative without one
 * @from_day: the first day covered, YYYY-MM-DD, for the header
 * @to_day: the last day covered, inclusive
 *
 * The calendar as an org-mode file for org-agenda: one heading per item.
 * A due date -- a ticket, an activity, a milestone, a goal, a deal close,
 * a bill -- is a `TODO` (or `DONE`) heading with a `DEADLINE:`; an event,
 * a sprint or a synced meeting is a plain heading with an active
 * timestamp, which the agenda shows on its day. Times are written in
 * @timezone, as org timestamps carry none. Each heading keeps the item's
 * stable identifier as `:ID:`, so a refreshed file is the same entries.
 * Notes become `#` comment lines at the top: a feed missing a kind says so.
 *
 * Returns: (transfer full): the text
 */
gchar *
venture_calendar_result_to_org(
	const VentureCalendarResult	*result,
	GTimeZone			*timezone,
	const gchar			*base_url,
	const gchar			*from_day,
	const gchar			*to_day
);

/**
 * venture_calendar_register_builtin_sources:
 * @context: the context
 *
 * Registers the built-in sources: event, ticket, activity, milestone,
 * goal, sprint, deal, money and caldav.
 */
void venture_calendar_register_builtin_sources(VentureContext *context);

G_END_DECLS

#endif /* VENTURE_CALENDAR_VIEW_H */
