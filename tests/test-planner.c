/*
 * test-planner.c - Events, their run-sheets, and the one calendar
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The planner is worth having only if three things hold. An event's
 * times must mean what a person wrote: an all-day trip is days, not a
 * moment at 15:00 UTC, and an end before the start is refused rather than
 * drawn backwards. The run-sheet's times must follow from the minutes, so
 * lengthening one item moves every later one. And the calendar must be
 * the books' kind of honest: each source shows exactly what falls in the
 * span, a hidden module's records are absent, a narrowing a source cannot
 * answer is said rather than ignored, and past the row bound a source is
 * refused with a note -- never a silently shortened month.
 */

#include <venture.h>

#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>

#include "venture-test-util.h"

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 organization_id;
	gint64		 venture_id;
} Fixture;

#define ID(record) (venture_entity_get_id(VENTURE_ENTITY(record)))

#define ASSERT_CONTAINS(haystack, needle) G_STMT_START {                       \
	if (NULL == strstr((haystack), (needle)))                              \
		g_error("expected \"%s\" in: %s", (needle), (haystack));       \
} G_STMT_END

/* Saves, expecting success; the refusal's reason is the failure. */
static void
save(
	Fixture		*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	if (0 == venture_entity_get_organization_id(VENTURE_ENTITY(record)))
		venture_entity_set_organization_id(VENTURE_ENTITY(record), fixture->organization_id);

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}

/* Saves, expecting a refusal whose message holds @fragment. */
static void
save_refused(
	Fixture		*fixture,
	gpointer	 record,
	const gchar	*fragment
){
	g_autoptr(GError) error = NULL;
	gboolean saved;

	if (0 == venture_entity_get_organization_id(VENTURE_ENTITY(record)))
		venture_entity_set_organization_id(VENTURE_ENTITY(record), fixture->organization_id);

	saved = venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error);
	g_assert_nonnull(error);
	g_assert_false(saved);
	ASSERT_CONTAINS(error->message, fragment);
}

/* Sets a field from its text, as a form or the API would. */
static void
field(
	gpointer	 record,
	const gchar	*name,
	const gchar	*value
){
	g_autoptr(GError) error = NULL;

	if (!venture_entity_set_field_from_string(VENTURE_ENTITY(record), name, value, &error))
		g_error("%s=%s: %s", name, value, error->message);
}

static GDateTime *
at(const gchar *text)
{
	GDateTime *when;

	when = venture_time_from_string(text, NULL);
	g_assert_nonnull(when);

	return when;
}

static void
assert_time(
	GDateTime	*when,
	const gchar	*expected
){
	g_autoptr(GDateTime) want = at(expected);

	g_assert_nonnull(when);

	if (!venture_time_equal(when, want))
	{
		g_autofree gchar *got = venture_time_to_string(when);

		g_error("expected %s, got %s", expected, got);
	}
}

static void
reset_modules(void)
{
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	/* The entity registry is process-wide; a test that switched a module
	 * off must not leave it off for the next. */
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureVenture) venture = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->organization_id =
		venture_context_get_default_organization_id(fixture->context);

	venture = venture_venture_new();
	g_object_set(venture, "name", "Workshop", NULL);
	save(fixture, venture);
	fixture->venture_id = ID(venture);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
	venture_aggregate_set_max_rows(0);
	reset_modules();
}

/* --- Records --------------------------------------------------------------- */

static VentureEntity *
event_new(
	Fixture		*fixture,
	const gchar	*name,
	const gchar	*starts,
	const gchar	*ends
){
	VentureEntity *event;

	event = VENTURE_ENTITY(venture_event_new());
	g_object_set(event, "name", name, NULL);
	venture_entity_set_organization_id(event, fixture->organization_id);

	if (NULL != starts)
		field(event, "starts-at", starts);

	if (NULL != ends)
		field(event, "ends-at", ends);

	return event;
}

static gint64
ticket(
	Fixture		*fixture,
	const gchar	*title,
	const gchar	*due,
	const gchar	*status
){
	g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_ticket_new());

	g_object_set(row, "title", title, NULL);

	if (NULL != due)
		field(row, "due-at", due);

	if (NULL != status)
		field(row, "status", status);

	save(fixture, row);

	return ID(row);
}

/* The calendar for [from, to), every source unless @kinds names some. */
static VentureCalendarResult *
calendar(
	Fixture		*fixture,
	const gchar	*from,
	const gchar	*to,
	const gchar	*const *kinds,
	gint64		 venture_id,
	gboolean	 include_done
){
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) start = at(from);
	g_autoptr(GDateTime) end = at(to);
	g_autoptr(GDateTime) now = at("2026-03-01");
	VentureCalendarQuery query;
	VentureCalendarResult *result;

	venture_calendar_query_init(&query);
	query.from = start;
	query.to = end;
	query.kinds = (gchar **)kinds;
	query.venture_id = venture_id;
	query.include_done = include_done;
	query.organization_ids = &fixture->organization_id;
	query.n_organizations = 1;
	query.now = now;

	result = venture_calendar_view_fetch(fixture->context, &query, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);

	return result;
}

/* How many items of @kind are titled @title (NULL: any title). */
static guint
items(
	VentureCalendarResult	*result,
	const gchar		*kind,
	const gchar		*title
){
	guint i;
	guint n;

	n = 0;

	for (i = 0; i < result->items->len; i++)
	{
		VentureCalendarItem *item = g_ptr_array_index(result->items, i);

		if ((NULL != kind) && (0 != g_strcmp0(item->kind, kind)))
			continue;

		if ((NULL != title) && (0 != g_strcmp0(item->title, title)))
			continue;

		n++;
	}

	return n;
}

static VentureCalendarItem *
item_titled(
	VentureCalendarResult	*result,
	const gchar		*title
){
	guint i;

	for (i = 0; i < result->items->len; i++)
	{
		VentureCalendarItem *item = g_ptr_array_index(result->items, i);

		if (0 == g_strcmp0(item->title, title))
			return item;
	}

	g_error("no item titled %s", title);
	return NULL;
}

static gboolean
has_note(
	VentureCalendarResult	*result,
	const gchar		*fragment
){
	guint i;

	for (i = 0; i < result->notes->len; i++)
	{
		if (NULL != strstr(g_ptr_array_index(result->notes, i), fragment))
			return TRUE;
	}

	return FALSE;
}

/* ==========================================================================
 * Event rules
 * ========================================================================== */

/*
 * What breaks without these: an event drawn backwards across a week, an
 * all-day trip that sits at 15:00 UTC and lands on the wrong day in the
 * feed, and a "repeats until" on an event that never repeats -- which a
 * person reads as a series that is not there.
 */
static void
test_event_rules(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) nowhen = NULL;
	g_autoptr(VentureEntity) backwards = NULL;
	g_autoptr(VentureEntity) trip = NULL;
	g_autoptr(VentureEntity) day = NULL;
	g_autoptr(VentureEntity) short_trip = NULL;
	g_autoptr(VentureEntity) until = NULL;
	g_autoptr(VentureEntity) early = NULL;
	g_autoptr(VentureEntity) moment = NULL;
	g_autoptr(GDateTime) starts = NULL;
	g_autoptr(GDateTime) ends = NULL;

	(void)user_data;

	nowhen = event_new(fixture, "Undated", NULL, NULL);
	save_refused(fixture, nowhen, "Starts");

	backwards = event_new(fixture, "Backwards", "2026-03-02T10:00:00Z", "2026-03-02T09:00:00Z");
	save_refused(fixture, backwards, "before the start");

	/* A moment -- a deadline -- has an end equal to its start. */
	moment = event_new(fixture, "Filing deadline", "2026-03-02T17:00:00Z", "2026-03-02T17:00:00Z");
	field(moment, "kind", "deadline");
	save(fixture, moment);

	/* All day: times become dates, the end the last day inclusive. */
	trip = event_new(fixture, "Fair", "2026-03-10T15:00:00Z", "2026-03-12T09:30:00Z");
	g_object_set(trip, "all-day", TRUE, NULL);
	field(trip, "kind", "trip");
	save(fixture, trip);
	g_object_get(trip, "starts-at", &starts, "ends-at", &ends, NULL);
	assert_time(starts, "2026-03-10");
	assert_time(ends, "2026-03-12");

	/* An all-day event with no end is its one day. */
	g_clear_pointer(&ends, g_date_time_unref);
	day = event_new(fixture, "Holiday", "2026-03-20", NULL);
	g_object_set(day, "all-day", TRUE, NULL);
	save(fixture, day);
	g_object_get(day, "ends-at", &ends, NULL);
	assert_time(ends, "2026-03-20");

	short_trip = event_new(fixture, "Inverted trip", "2026-03-10", "2026-03-09");
	g_object_set(short_trip, "all-day", TRUE, NULL);
	save_refused(fixture, short_trip, "before the first day");

	until = event_new(fixture, "Once", "2026-03-02T09:00:00Z", NULL);
	field(until, "recurs-until", "2026-04-01");
	save_refused(fixture, until, "only for an event that repeats");

	early = event_new(fixture, "Standup", "2026-03-02T09:00:00Z", NULL);
	field(early, "recurrence", "daily");
	field(early, "recurs-until", "2026-02-01");
	save_refused(fixture, early, "before the event's first day");
}

/*
 * An attendee is somebody: a row naming nobody is refused, a name alone
 * is enough, and an address must look like one. What breaks: an
 * attendee list of blank rows counted as "waiting".
 */
static void
test_attendee_rules(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) nobody = NULL;
	g_autoptr(VentureEntity) named = NULL;
	g_autoptr(VentureEntity) bad_mail = NULL;
	g_autoptr(VentureEntity) orphan = NULL;
	g_autoptr(GPtrArray) people = NULL;
	guint counts[VENTURE_PLANNER_RSVP_COUNT];

	(void)user_data;

	event = event_new(fixture, "Board", "2026-03-02T09:00:00Z", "2026-03-02T10:00:00Z");
	save(fixture, event);

	nobody = VENTURE_ENTITY(venture_event_attendee_new());
	g_object_set(nobody, "event-id", ID(event), NULL);
	save_refused(fixture, nobody, "an attendee has to be somebody");

	bad_mail = VENTURE_ENTITY(venture_event_attendee_new());
	g_object_set(bad_mail, "event-id", ID(event), "email", "not-an-address", NULL);
	save_refused(fixture, bad_mail, "is not an email address");

	orphan = VENTURE_ENTITY(venture_event_attendee_new());
	g_object_set(orphan, "event-id", (gint64)9999, "name", "Ghost", NULL);
	venture_entity_set_organization_id(orphan, fixture->organization_id);
	{
		g_autoptr(GError) error = NULL;

		g_assert_false(venture_database_save(fixture->database, orphan, NULL, &error));
		g_assert_nonnull(error);
	}

	named = VENTURE_ENTITY(venture_event_attendee_new());
	g_object_set(named, "event-id", ID(event), "name", "Ada", NULL);
	field(named, "rsvp", "yes");
	save(fixture, named);

	/* The answers counted in core, for the page and the API alike. */
	people = venture_planner_attendees(fixture->database, event, counts, NULL);
	g_assert_nonnull(people);
	g_assert_cmpuint(people->len, ==, 1);
	g_assert_cmpuint(counts[VENTURE_ATTENDEE_RSVP_YES], ==, 1);
	g_assert_cmpuint(counts[VENTURE_ATTENDEE_RSVP_PENDING], ==, 0);
}

/*
 * The run-sheet's times are the event's start plus the minutes before:
 * 09:00, 09:15, 09:45 for 15, 30 and 20 minutes, in position order not
 * id order, and 65 minutes overrun an hour. Lengthening the first item
 * moves every later one. What breaks: stored start times that go stale
 * the moment one item changes, or a sheet in the order rows were added.
 */
static void
test_agenda_running_times(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) welcome = NULL;
	g_autoptr(VentureEntity) numbers = NULL;
	g_autoptr(VentureEntity) questions = NULL;
	g_autoptr(VentureEntity) negative = NULL;
	g_autoptr(VenturePlannerAgenda) agenda = NULL;
	g_autoptr(VenturePlannerAgenda) again = NULL;
	g_autoptr(VentureEntity) all_day = NULL;
	g_autoptr(VenturePlannerAgenda) days = NULL;
	VenturePlannerAgendaLine *line;

	(void)user_data;

	event = event_new(fixture, "Review", "2026-03-02T09:00:00Z", "2026-03-02T10:00:00Z");
	save(fixture, event);

	/* Added out of order: the sheet follows position. */
	questions = VENTURE_ENTITY(venture_event_agenda_item_new());
	g_object_set(questions, "event-id", ID(event), "title", "Questions",
	             "position", (gint64)3, "minutes", (gint64)20, NULL);
	save(fixture, questions);
	welcome = VENTURE_ENTITY(venture_event_agenda_item_new());
	g_object_set(welcome, "event-id", ID(event), "title", "Welcome",
	             "position", (gint64)1, "minutes", (gint64)15, NULL);
	save(fixture, welcome);
	numbers = VENTURE_ENTITY(venture_event_agenda_item_new());
	g_object_set(numbers, "event-id", ID(event), "title", "Numbers",
	             "position", (gint64)2, "minutes", (gint64)30, NULL);
	save(fixture, numbers);

	negative = VENTURE_ENTITY(venture_event_agenda_item_new());
	g_object_set(negative, "event-id", ID(event), "title", "Rewind",
	             "minutes", (gint64)-5, NULL);
	save_refused(fixture, negative, "cannot be negative");

	agenda = venture_planner_agenda(fixture->database, event, NULL);
	g_assert_nonnull(agenda);
	g_assert_cmpuint(agenda->lines->len, ==, 3);
	g_assert_cmpint(agenda->total_minutes, ==, 65);
	g_assert_true(agenda->overruns);

	line = g_ptr_array_index(agenda->lines, 0);
	g_assert_cmpint(ID(line->item), ==, ID(welcome));
	assert_time(line->starts, "2026-03-02T09:00:00Z");
	line = g_ptr_array_index(agenda->lines, 1);
	assert_time(line->starts, "2026-03-02T09:15:00Z");
	line = g_ptr_array_index(agenda->lines, 2);
	assert_time(line->starts, "2026-03-02T09:45:00Z");
	assert_time(line->ends, "2026-03-02T10:05:00Z");

	/* Ten more minutes of welcome move everything after it. */
	g_object_set(welcome, "minutes", (gint64)25, NULL);
	save(fixture, welcome);
	again = venture_planner_agenda(fixture->database, event, NULL);
	line = g_ptr_array_index(again->lines, 2);
	assert_time(line->starts, "2026-03-02T09:55:00Z");
	g_assert_cmpint(line->offset_minutes, ==, 55);

	/* An all-day event's sheet has offsets and no clock. */
	all_day = event_new(fixture, "Offsite", "2026-03-05", NULL);
	g_object_set(all_day, "all-day", TRUE, NULL);
	save(fixture, all_day);
	g_clear_object(&welcome);
	welcome = VENTURE_ENTITY(venture_event_agenda_item_new());
	g_object_set(welcome, "event-id", ID(all_day), "title", "Coffee",
	             "minutes", (gint64)30, NULL);
	save(fixture, welcome);
	days = venture_planner_agenda(fixture->database, all_day, NULL);
	line = g_ptr_array_index(days->lines, 0);
	g_assert_null(line->starts);
	g_assert_false(days->overruns);
}

/*
 * Preparation tasks are ordinary tickets naming the event: two of three
 * live tickets, one done, and a cancelled one counted as neither. What
 * breaks: "1 of 3 done" for work that was called off.
 */
static void
test_prep_tickets(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(GPtrArray) tickets = NULL;
	const gchar *const statuses[] = { "done", "todo", "cancelled" };
	guint done;
	guint total;
	guint i;

	(void)user_data;

	event = event_new(fixture, "Launch party", "2026-03-20T18:00:00Z", NULL);
	save(fixture, event);

	for (i = 0; i < G_N_ELEMENTS(statuses); i++)
	{
		g_autoptr(VentureEntity) row = VENTURE_ENTITY(venture_ticket_new());

		g_object_set(row, "title", statuses[i], "event-id", ID(event), NULL);
		field(row, "status", statuses[i]);
		save(fixture, row);
	}

	tickets = venture_planner_prep_tickets(fixture->database, event, &done, &total, NULL);
	g_assert_nonnull(tickets);
	g_assert_cmpuint(tickets->len, ==, 3);
	g_assert_cmpuint(done, ==, 1);
	g_assert_cmpuint(total, ==, 2);

	/* A ticket naming an event that does not exist is refused by the
	 * generic reference check, like any reference. */
	{
		g_autoptr(VentureEntity) stray = VENTURE_ENTITY(venture_ticket_new());

		g_object_set(stray, "title", "Stray", "event-id", (gint64)4242, NULL);
		save_refused(fixture, stray, "4242");
	}
}

/* ==========================================================================
 * The calendar's sources
 * ========================================================================== */

/*
 * Every built-in source, each with one record inside March and one in
 * April: March shows exactly the inside ones. What breaks: a source that
 * ignores the span (April's deal in March), or one that reads the wrong
 * date field.
 */
static void
test_sources_in_and_out(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureCalendarResult) march = NULL;
	g_autoptr(VentureCalendarResult) april = NULL;
	const gchar *const months[][2] = {
		{ "March", "2026-03-12" },
		{ "April", "2026-04-12" }
	};
	guint m;

	(void)user_data;

	for (m = 0; m < G_N_ELEMENTS(months); m++)
	{
		g_autoptr(VentureEntity) event = NULL;
		g_autoptr(VentureEntity) activity = NULL;
		g_autoptr(VentureEntity) milestone = NULL;
		g_autoptr(VentureEntity) goal = NULL;
		g_autoptr(VentureEntity) sprint = NULL;
		g_autoptr(VentureEntity) deal = NULL;
		g_autoptr(VentureEntity) synced = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *timed = g_strdup_printf("%sT10:00:00Z", months[m][1]);
		g_autofree gchar *later = g_strdup_printf("%sT11:00:00Z", months[m][1]);

		name = g_strdup_printf("%s event", months[m][0]);
		event = event_new(fixture, name, timed, later);
		save(fixture, event);

		g_clear_pointer(&name, g_free);
		name = g_strdup_printf("%s ticket", months[m][0]);
		ticket(fixture, name, months[m][1], NULL);

		activity = VENTURE_ENTITY(venture_activity_new());
		g_clear_pointer(&name, g_free);
		name = g_strdup_printf("%s follow-up", months[m][0]);
		g_object_set(activity, "subject", name, NULL);
		field(activity, "kind", "followup");
		field(activity, "due-at", months[m][1]);
		save(fixture, activity);

		milestone = VENTURE_ENTITY(venture_milestone_new());
		g_clear_pointer(&name, g_free);
		name = g_strdup_printf("%s milestone", months[m][0]);
		g_object_set(milestone, "name", name, NULL);
		field(milestone, "due-on", months[m][1]);
		save(fixture, milestone);

		goal = VENTURE_ENTITY(venture_goal_new());
		g_clear_pointer(&name, g_free);
		name = g_strdup_printf("%s goal", months[m][0]);
		g_object_set(goal, "name", name, "start-value", 0.0, "target-value", 10.0, NULL);
		field(goal, "due-on", months[m][1]);
		save(fixture, goal);

		sprint = VENTURE_ENTITY(venture_sprint_new());
		g_clear_pointer(&name, g_free);
		name = g_strdup_printf("%s sprint", months[m][0]);
		g_object_set(sprint, "name", name, NULL);
		field(sprint, "starts-on", months[m][1]);
		field(sprint, "ends-on", months[m][1]);
		save(fixture, sprint);

		deal = VENTURE_ENTITY(venture_deal_new());
		g_clear_pointer(&name, g_free);
		name = g_strdup_printf("%s deal", months[m][0]);
		g_object_set(deal, "name", name, NULL);
		field(deal, "expected-close-at", months[m][1]);
		save(fixture, deal);

		synced = VENTURE_ENTITY(venture_calendar_event_new());
		g_clear_pointer(&name, g_free);
		name = g_strdup_printf("%s synced", months[m][0]);
		g_object_set(synced, "subject", name, NULL);
		field(synced, "starts-at", timed);
		field(synced, "ends-at", later);
		save(fixture, synced);
	}

	march = calendar(fixture, "2026-03-01", "2026-04-01", NULL, 0, FALSE);
	april = calendar(fixture, "2026-04-01", "2026-05-01", NULL, 0, FALSE);

	g_assert_cmpuint(items(march, "event", "March event"), ==, 1);
	g_assert_cmpuint(items(march, "ticket", "March ticket"), ==, 1);
	g_assert_cmpuint(items(march, "activity", "March follow-up"), ==, 1);
	g_assert_cmpuint(items(march, "milestone", "March milestone"), ==, 1);
	g_assert_cmpuint(items(march, "goal", "March goal"), ==, 1);
	g_assert_cmpuint(items(march, "sprint", "March sprint"), ==, 1);
	g_assert_cmpuint(items(march, "deal", "March deal"), ==, 1);
	g_assert_cmpuint(items(march, "caldav", "March synced"), ==, 1);

	/* Nothing of April's in March, and the other way round. */
	g_assert_cmpuint(items(march, NULL, "April event") + items(march, NULL, "April ticket") +
	                 items(march, NULL, "April deal") + items(march, NULL, "April goal") +
	                 items(march, NULL, "April sprint") + items(march, NULL, "April synced"), ==, 0);
	g_assert_cmpuint(items(april, "deal", "April deal"), ==, 1);
	g_assert_cmpuint(items(april, NULL, "March milestone"), ==, 0);

	/* A date is a whole day; a time is a moment on its day. */
	g_assert_true(item_titled(march, "March milestone")->all_day);
	g_assert_cmpstr(item_titled(march, "March milestone")->day, ==, "2026-03-12");
	g_assert_false(item_titled(march, "March event")->all_day);
	{
		g_autofree gchar *url = g_strdup_printf("/e/event/%" G_GINT64_FORMAT,
		                                        item_titled(march, "March event")->record_id);

		g_assert_cmpstr(item_titled(march, "March event")->url, ==, url);
	}
}

/*
 * A multi-day event that began before the span is on it, and is on each
 * of its days; a timed meeting that ended exactly at the span's start is
 * not. What breaks: a trip that vanishes from the month it ends in.
 */
static void
test_spans_overlap(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) trip = NULL;
	g_autoptr(VentureEntity) late = NULL;
	g_autoptr(VentureCalendarResult) march = NULL;
	VentureCalendarItem *item;

	(void)user_data;

	trip = event_new(fixture, "Crossing", "2026-02-27", "2026-03-02");
	g_object_set(trip, "all-day", TRUE, NULL);
	save(fixture, trip);
	late = event_new(fixture, "Late meeting", "2026-02-28T23:00:00Z", "2026-03-01T00:00:00Z");
	save(fixture, late);

	march = calendar(fixture, "2026-03-01", "2026-04-01", NULL, 0, FALSE);
	g_assert_cmpuint(items(march, "event", "Crossing"), ==, 1);
	g_assert_cmpuint(items(march, "event", "Late meeting"), ==, 0);

	item = item_titled(march, "Crossing");
	g_assert_true(venture_calendar_item_covers_day(item, "2026-03-01", NULL));
	g_assert_true(venture_calendar_item_covers_day(item, "2026-03-02", NULL));
	g_assert_false(venture_calendar_item_covers_day(item, "2026-03-03", NULL));
	assert_time(item->end, "2026-03-03");
}

/*
 * The kind filter answers only what it names; a name nobody registered
 * is refused rather than read as "nothing". Done tickets show only when
 * asked for. What breaks: a typo in a kind that shows an empty month.
 */
static void
test_kind_filter(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureCalendarResult) only_tickets = NULL;
	g_autoptr(VentureCalendarResult) with_done = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) from = at("2026-03-01");
	g_autoptr(GDateTime) to = at("2026-04-01");
	const gchar *const tickets_only[] = { "ticket", NULL };
	const gchar *const typo[] = { "tickets", NULL };
	VentureCalendarQuery query;

	(void)user_data;

	event = event_new(fixture, "Meeting", "2026-03-03T10:00:00Z", NULL);
	save(fixture, event);
	ticket(fixture, "Open", "2026-03-04", NULL);
	ticket(fixture, "Finished", "2026-03-05", "done");

	only_tickets = calendar(fixture, "2026-03-01", "2026-04-01", tickets_only, 0, FALSE);
	g_assert_cmpuint(items(only_tickets, NULL, NULL), ==, 1);
	g_assert_cmpuint(items(only_tickets, "ticket", "Open"), ==, 1);
	g_assert_cmpuint(only_tickets->kinds->len, ==, 1);

	with_done = calendar(fixture, "2026-03-01", "2026-04-01", tickets_only, 0, TRUE);
	g_assert_cmpuint(items(with_done, "ticket", NULL), ==, 2);

	venture_calendar_query_init(&query);
	query.from = from;
	query.to = to;
	query.kinds = (gchar **)typo;
	query.organization_ids = &fixture->organization_id;
	query.n_organizations = 1;
	g_assert_null(venture_calendar_view_fetch(fixture->context, &query, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	ASSERT_CONTAINS(error->message, "is not a calendar source");
}

/*
 * A venture's calendar leaves out what carries no venture, and says so.
 * What breaks: every unrelated follow-up shown on one venture's month.
 */
static void
test_venture_filter(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) mine = NULL;
	g_autoptr(VentureEntity) theirs = NULL;
	g_autoptr(VentureEntity) activity = NULL;
	g_autoptr(VentureCalendarResult) result = NULL;

	(void)user_data;

	mine = event_new(fixture, "Ours", "2026-03-03T10:00:00Z", NULL);
	g_object_set(mine, "venture-id", fixture->venture_id, NULL);
	save(fixture, mine);
	theirs = event_new(fixture, "Elsewhere", "2026-03-03T11:00:00Z", NULL);
	save(fixture, theirs);
	activity = VENTURE_ENTITY(venture_activity_new());
	g_object_set(activity, "subject", "Call back", NULL);
	field(activity, "kind", "call");
	field(activity, "due-at", "2026-03-04");
	save(fixture, activity);

	result = calendar(fixture, "2026-03-01", "2026-04-01", NULL, fixture->venture_id, FALSE);
	g_assert_cmpuint(items(result, "event", "Ours"), ==, 1);
	g_assert_cmpuint(items(result, "event", "Elsewhere"), ==, 0);
	g_assert_cmpuint(items(result, "activity", NULL), ==, 0);
	g_assert_true(has_note(result, "Activities: left out, because they carry no venture"));
}

/*
 * A module switched off takes its source with it: goals off, no goal
 * items and no "goal" kind ran, and asked by name it says it is off.
 * Planner off refuses the calendar outright. What breaks: a hidden
 * module's records leaking onto the calendar, or an empty answer that
 * reads as "nothing due".
 */
static void
test_module_off(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) goal = NULL;
	g_autoptr(VentureCalendarResult) all = NULL;
	g_autoptr(VentureCalendarResult) named = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) available = NULL;
	g_autoptr(GDateTime) from = at("2026-03-01");
	g_autoptr(GDateTime) to = at("2026-04-01");
	const gchar *const goals_only[] = { "goal", NULL };
	VentureCalendarQuery query;

	(void)user_data;

	goal = VENTURE_ENTITY(venture_goal_new());
	g_object_set(goal, "name", "Save up", "start-value", 0.0, "target-value", 5.0, NULL);
	field(goal, "due-on", "2026-03-15");
	save(fixture, goal);

	venture_config_set_module_enabled(fixture->config, "goals", FALSE);
	g_assert_false(venture_context_module_enabled(fixture->context, "goals"));

	available = venture_calendar_sources_available(fixture->context);
	g_assert_false(g_strv_contains((const gchar *const *)available, "goal"));
	g_assert_true(g_strv_contains((const gchar *const *)available, "event"));

	all = calendar(fixture, "2026-03-01", "2026-04-01", NULL, 0, FALSE);
	g_assert_cmpuint(items(all, "goal", NULL), ==, 0);
	g_assert_false(g_ptr_array_find_with_equal_func(all->kinds, "goal", g_str_equal, NULL));

	named = calendar(fixture, "2026-03-01", "2026-04-01", goals_only, 0, FALSE);
	g_assert_true(has_note(named, "Goals: not available"));

	venture_config_set_module_enabled(fixture->config, "planner", FALSE);
	venture_calendar_query_init(&query);
	query.from = from;
	query.to = to;
	query.organization_ids = &fixture->organization_id;
	query.n_organizations = 1;
	g_assert_null(venture_calendar_view_fetch(fixture->context, &query, &error));
	ASSERT_CONTAINS(error->message, "planner module");
}

/*
 * Past the row bound a source is refused with a note and shows nothing;
 * the others still answer. Three tickets with the bound at two. What
 * breaks: a month that silently draws the first N tickets and looks
 * complete.
 */
static void
test_row_bound(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureCalendarResult) result = NULL;

	(void)user_data;

	event = event_new(fixture, "Still here", "2026-03-02T10:00:00Z", NULL);
	save(fixture, event);
	ticket(fixture, "One", "2026-03-03", NULL);
	ticket(fixture, "Two", "2026-03-04", NULL);
	ticket(fixture, "Three", "2026-03-05", NULL);

	venture_aggregate_set_max_rows(2);
	result = calendar(fixture, "2026-03-01", "2026-04-01", NULL, 0, FALSE);
	g_assert_cmpuint(items(result, "ticket", NULL), ==, 0);
	g_assert_true(has_note(result, "Tickets: more than 2"));
	g_assert_cmpuint(items(result, "event", "Still here"), ==, 1);
}

/*
 * A repeating event is one record, expanded in the span: a weekly
 * Monday meeting begun in 2025 is on March 2026's five Mondays, three
 * once it stops on the 16th; a monthly on the 31st is clamped to each
 * month's end; a daily one over a year stays inside the bound; and a
 * span past VENTURE_CALENDAR_MAX_DAYS is refused. What breaks: an
 * expansion that walks from 2025 forever, or a feed whose occurrences
 * share one UID and collapse into one.
 */
static void
test_recurrence(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) weekly = NULL;
	g_autoptr(VentureEntity) monthly = NULL;
	g_autoptr(VentureEntity) daily = NULL;
	g_autoptr(VentureCalendarResult) march = NULL;
	g_autoptr(VentureCalendarResult) april = NULL;
	g_autoptr(VentureCalendarResult) year = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) from = at("2026-01-01");
	g_autoptr(GDateTime) to = at("2027-03-01");
	const gchar *const events_only[] = { "event", NULL };
	VentureCalendarQuery query;
	VentureCalendarItem *first;
	VentureCalendarItem *second;
	guint i;

	(void)user_data;

	weekly = event_new(fixture, "Weekly", "2025-01-06T09:00:00Z", "2025-01-06T09:30:00Z");
	field(weekly, "recurrence", "weekly");
	save(fixture, weekly);

	monthly = event_new(fixture, "Month end", "2026-01-31T12:00:00Z", NULL);
	field(monthly, "recurrence", "monthly");
	save(fixture, monthly);

	march = calendar(fixture, "2026-03-01", "2026-04-01", events_only, 0, FALSE);
	g_assert_cmpuint(items(march, "event", "Weekly"), ==, 5);
	g_assert_cmpuint(items(march, "event", "Month end"), ==, 1);
	assert_time(item_titled(march, "Month end")->start, "2026-03-31T12:00:00Z");

	/* Occurrences carry their own feed identity. */
	first = NULL;
	second = NULL;

	for (i = 0; i < march->items->len; i++)
	{
		VentureCalendarItem *item = g_ptr_array_index(march->items, i);

		if (0 != g_strcmp0(item->title, "Weekly"))
			continue;

		if (NULL == first)
			first = item;
		else if (NULL == second)
			second = item;
	}

	g_assert_cmpstr(first->uid, !=, second->uid);
	assert_time(first->start, "2026-03-02T09:00:00Z");
	assert_time(first->end, "2026-03-02T09:30:00Z");

	/* February has no 31st: the 28th. */
	april = calendar(fixture, "2026-02-01", "2026-03-01", events_only, 0, FALSE);
	assert_time(item_titled(april, "Month end")->start, "2026-02-28T12:00:00Z");

	field(weekly, "recurs-until", "2026-03-16");
	save(fixture, weekly);
	g_clear_pointer(&march, venture_calendar_result_free);
	march = calendar(fixture, "2026-03-01", "2026-04-01", events_only, 0, FALSE);
	g_assert_cmpuint(items(march, "event", "Weekly"), ==, 3);

	daily = event_new(fixture, "Daily", "2020-01-01T08:00:00Z", NULL);
	field(daily, "recurrence", "daily");
	save(fixture, daily);
	year = calendar(fixture, "2026-01-01", "2027-01-01", events_only, 0, FALSE);
	g_assert_cmpuint(items(year, "event", "Daily"), ==, 365);

	venture_calendar_query_init(&query);
	query.from = from;
	query.to = to;
	query.organization_ids = &fixture->organization_id;
	query.n_organizations = 1;
	g_assert_null(venture_calendar_view_fetch(fixture->context, &query, &error));
	ASSERT_CONTAINS(error->message, "at most");
}

/*
 * The feed: one VEVENT per item, an all-day item as DATE values with an
 * exclusive end, a timed one in UTC, a cancelled event marked so. What
 * breaks: a subscribed calendar that draws a trip as a moment, or a
 * cancelled meeting as going ahead.
 */
static void
test_ics(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) trip = NULL;
	g_autoptr(VentureEntity) call = NULL;
	g_autoptr(VentureEntity) off = NULL;
	g_autoptr(VentureCalendarResult) result = NULL;
	g_autofree gchar *ics = NULL;
	const gchar *cursor;
	guint vevents;

	(void)user_data;

	trip = event_new(fixture, "Expo", "2026-03-10", "2026-03-12");
	g_object_set(trip, "all-day", TRUE, NULL);
	save(fixture, trip);
	call = event_new(fixture, "Call", "2026-03-04T15:30:00Z", "2026-03-04T16:00:00Z");
	save(fixture, call);
	off = event_new(fixture, "Called off", "2026-03-05T15:30:00Z", NULL);
	field(off, "status", "cancelled");
	save(fixture, off);

	result = calendar(fixture, "2026-03-01", "2026-04-01", NULL, 0, TRUE);
	ics = venture_calendar_result_to_ics(result);

	g_assert_true(g_str_has_prefix(ics, "BEGIN:VCALENDAR\r\n"));
	g_assert_true(g_str_has_suffix(ics, "END:VCALENDAR\r\n"));
	ASSERT_CONTAINS(ics, "DTSTART;VALUE=DATE:20260310\r\n");
	ASSERT_CONTAINS(ics, "DTEND;VALUE=DATE:20260313\r\n");
	ASSERT_CONTAINS(ics, "DTSTART:20260304T153000Z\r\n");
	ASSERT_CONTAINS(ics, "SUMMARY:Called off\r\n");
	ASSERT_CONTAINS(ics, "STATUS:CANCELLED\r\n");

	vevents = 0;

	for (cursor = strstr(ics, "BEGIN:VEVENT"); NULL != cursor; cursor = strstr(cursor + 1, "BEGIN:VEVENT"))
		vevents++;

	g_assert_cmpuint(vevents, ==, result->items->len);
	g_assert_cmpuint(vevents, ==, 3);
}

/*
 * The span a view covers: a month from the 1st, a week from its Monday,
 * an explicit span inclusive of both days, and a backwards one refused.
 * What breaks: a week view starting on a Sunday, or "to" read as
 * exclusive so the last day is never shown.
 */
static void
test_view_range(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GDateTime) from = NULL;
	g_autoptr(GDateTime) to = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	g_assert_true(venture_calendar_view_range(fixture->context, "month", "2026-03-18", NULL, NULL,
	                                          &from, &to, &error));
	assert_time(from, "2026-03-01");
	assert_time(to, "2026-04-01");
	g_clear_pointer(&from, g_date_time_unref);
	g_clear_pointer(&to, g_date_time_unref);

	/* 18 March 2026 is a Wednesday; its week starts on the 16th. */
	g_assert_true(venture_calendar_view_range(fixture->context, "week", "2026-03-18", NULL, NULL,
	                                          &from, &to, &error));
	assert_time(from, "2026-03-16");
	assert_time(to, "2026-03-23");
	g_clear_pointer(&from, g_date_time_unref);
	g_clear_pointer(&to, g_date_time_unref);

	g_assert_true(venture_calendar_view_range(fixture->context, "agenda", NULL, "2026-03-01", "2026-03-31",
	                                          &from, &to, &error));
	assert_time(to, "2026-04-01");
	g_clear_pointer(&from, g_date_time_unref);
	g_clear_pointer(&to, g_date_time_unref);

	g_assert_false(venture_calendar_view_range(fixture->context, "agenda", NULL, "2026-03-31", "2026-03-01",
	                                           &from, &to, &error));
	ASSERT_CONTAINS(error->message, "before from");
	g_clear_error(&error);

	g_assert_false(venture_calendar_view_range(fixture->context, "year", NULL, NULL, NULL,
	                                           &from, &to, &error));
	ASSERT_CONTAINS(error->message, "month, week or agenda");
}

/*
 * The money calendar answers through its own expander: an approved bill
 * due in March is a money item in March, and with the money calendar
 * switched off the source is gone. What breaks: a second copy of its
 * logic that disagrees with /money/calendar.
 */
static void
test_money_source(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) vendor = NULL;
	g_autoptr(VentureEntity) year = NULL;
	g_autoptr(VentureEntity) bill = NULL;
	g_autoptr(VentureEntity) line = NULL;
	g_autoptr(VentureEntity) approve = NULL;
	g_autoptr(VentureCalendarResult) result = NULL;
	g_autoptr(VentureCalendarResult) off = NULL;
	g_auto(GStrv) available = NULL;
	const gchar *const money_only[] = { "money", NULL };
	GType type;

	(void)user_data;

	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "company");
	vendor = g_object_new(type, "organization-id", fixture->organization_id, "name", "Cloud Host", NULL);
	field(vendor, "kind", "supplier");
	save(fixture, vendor);
	year = g_object_new(VENTURE_TYPE_FISCAL_YEAR, "organization-id", fixture->organization_id,
	                    "name", "2026", NULL);
	field(year, "start-at", "2026-01-01");
	save(fixture, year);

	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "vendor_bill");
	bill = g_object_new(type, "organization-id", fixture->organization_id, "number", "BILL-9",
	                    "company-id", ID(vendor), "currency", "USD", "status", "draft", NULL);
	field(bill, "bill-date", "2026-03-01");
	field(bill, "due-date", "2026-03-20");
	save(fixture, bill);
	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "vendor_bill_line");
	line = g_object_new(type, "organization-id", fixture->organization_id, "bill-id", ID(bill),
	                    "description", "Hosting", "quantity", "1", "category", "hosting", NULL);
	field(line, "unit-price", "100 USD");
	save(fixture, line);
	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "vendor_bill_event");
	approve = g_object_new(type, "organization-id", fixture->organization_id, "bill-id", ID(bill),
	                       "vendor-id", ID(vendor), "kind", "approve", "state", "approved", NULL);
	field(approve, "date", "2026-03-01");
	save(fixture, approve);

	result = calendar(fixture, "2026-03-01", "2026-04-01", money_only, 0, FALSE);
	g_assert_cmpuint(items(result, "money", NULL), ==, 1);
	g_assert_cmpstr(((VentureCalendarItem *)g_ptr_array_index(result->items, 0))->day, ==, "2026-03-20");
	g_assert_cmpstr(((VentureCalendarItem *)g_ptr_array_index(result->items, 0))->record_type, ==, "vendor_bill");

	venture_config_set_module_enabled(fixture->config, "money_calendar", FALSE);
	available = venture_calendar_sources_available(fixture->context);
	g_assert_false(g_strv_contains((const gchar *const *)available, "money"));
	off = calendar(fixture, "2026-03-01", "2026-04-01", NULL, 0, FALSE);
	g_assert_cmpuint(items(off, "money", NULL), ==, 0);
}

/*
 * A plugin adds a source the same way the built-ins do, and taking it
 * back removes it. What breaks: a calendar whose sources are a list in
 * the page, which no plugin can extend.
 */
static gboolean
test_source_fetch(
	VentureContext			 *context,
	const VentureCalendarQuery	 *query,
	VentureCalendarResult		 *result,
	gpointer			  user_data,
	GError				**error
){
	(void)context;
	(void)user_data;
	(void)error;

	venture_calendar_result_add(result,
		venture_calendar_item_new("harvest", "Harvest moon", query->from, NULL, 0));

	return TRUE;
}

static void
test_source_registry(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureCalendarResult) result = NULL;
	g_auto(GStrv) names = NULL;

	(void)user_data;

	venture_calendar_sources_add(fixture->context, "harvest", "Harvests", NULL, NULL,
	                             VENTURE_CALENDAR_SOURCE_NONE, test_source_fetch, NULL, NULL);
	result = calendar(fixture, "2026-03-01", "2026-04-01", NULL, 0, FALSE);
	g_assert_cmpuint(items(result, "harvest", "Harvest moon"), ==, 1);
	g_assert_cmpstr(venture_calendar_sources_get_label(fixture->context, "harvest"), ==, "Harvests");

	g_assert_true(venture_calendar_sources_remove(fixture->context, "harvest"));
	names = venture_calendar_sources_list(fixture->context);
	g_assert_false(g_strv_contains((const gchar *const *)names, "harvest"));
}

/* ==========================================================================
 * HTTP
 * ========================================================================== */

typedef struct
{
	VentureConfig		*config;
	VentureDatabase		*database;
	VentureContext		*context;
	VentureWebServer	*server;
	SoupSession		*session;
	gchar			*state_dir;
	gchar			*cookie;
	guint16			 port;
} ServerFixture;

typedef struct
{
	gboolean	 done;
	GBytes		*body;
	GError		*error;
} RequestResult;

static void
request_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	RequestResult *outcome;

	outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source),
	                                                  result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request(
	ServerFixture	 *fixture,
	const gchar	 *method,
	const gchar	 *path,
	const gchar	 *content_type,
	const gchar	 *body,
	gchar		**out_body
){
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };

	url = g_strdup_printf("http://127.0.0.1:%u%s", fixture->port, path);
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (NULL != fixture->cookie)
		soup_message_headers_append(
			soup_message_get_request_headers(message), "Cookie",
			fixture->cookie);

	if (NULL != body)
	{
		g_autoptr(GBytes) bytes = NULL;

		bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message, content_type, bytes);
	}

	soup_session_send_and_read_async(fixture->session, message,
	                                 G_PRIORITY_DEFAULT, NULL, request_done,
	                                 &outcome);

	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);

	if (NULL != outcome.error)
		g_error("%s %s: %s", method, path, outcome.error->message);

	if (NULL != out_body)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL),
		                      g_bytes_get_size(outcome.body));

	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);

	return soup_message_get_status(message);
}

static void
server_fixture_set_up(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) user = NULL;
	g_autofree gchar *set_cookie = NULL;
	gchar *semicolon;

	(void)user_data;

	g_setenv("VENTURE_TEST_SESSION_SECRET", "planner-test-secret", TRUE);

	fixture->state_dir = g_dir_make_tmp("venture-planner-XXXXXX", NULL);
	/* 0: the kernel picks a free port, read back after the start. */
	fixture->port = 0;

	fixture->config = venture_config_new();
	g_object_set(fixture->config,
	             "state-dir", fixture->state_dir,
	             "server-bind-address", "127.0.0.1",
	             "server-port", (gint64)fixture->port,
	             "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
	             "security-password-iterations", (gint64)100000,
	             "locale-timezone", "UTC",
	             NULL);

	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->server = venture_web_server_new(fixture->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
	fixture->port = venture_web_server_get_port(fixture->server);

	fixture->session = soup_session_new();

	user = venture_user_new();
	g_object_set(user, "username", "owner", "role", VENTURE_USER_ROLE_OWNER,
	             "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(user, "owner-password-1", 100000,
	                                        NULL));
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(user), NULL, NULL));

	{
		g_autoptr(SoupMessage) message = NULL;
		g_autofree gchar *url = NULL;
		g_autoptr(GBytes) bytes = NULL;
		RequestResult outcome = { FALSE, NULL, NULL };

		url = g_strdup_printf("http://127.0.0.1:%u/login", fixture->port);
		message = soup_message_new("POST", url);
		soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
		bytes = g_bytes_new_static("username=owner&password=owner-password-1",
		                           strlen("username=owner&password=owner-password-1"));
		soup_message_set_request_body_from_bytes(message,
			"application/x-www-form-urlencoded", bytes);
		soup_session_send_and_read_async(fixture->session, message,
		                                 G_PRIORITY_DEFAULT, NULL,
		                                 request_done, &outcome);

		while (!outcome.done)
			g_main_context_iteration(NULL, TRUE);

		g_clear_pointer(&outcome.body, g_bytes_unref);
		g_clear_error(&outcome.error);

		set_cookie = g_strdup(soup_message_headers_get_one(
			soup_message_get_response_headers(message), "Set-Cookie"));
	}

	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');

	if (NULL != semicolon)
		*semicolon = '\0';

	fixture->cookie = g_steal_pointer(&set_cookie);
}

static void
server_fixture_tear_down(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	(void)user_data;

	if (NULL != fixture->server)
		venture_web_server_stop(fixture->server);

	g_clear_pointer(&fixture->cookie, g_free);
	g_clear_object(&fixture->session);
	g_clear_object(&fixture->server);
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);

	if (NULL != fixture->state_dir)
	{
		venture_test_remove_tree(fixture->state_dir);
		g_clear_pointer(&fixture->state_dir, g_free);
	}

	g_unsetenv("VENTURE_TEST_SESSION_SECRET");

	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	venture_module_registry_configure(registry, everything, NULL);
	venture_module_registry_apply(registry,
	                              venture_entity_registry_get_default());
}

/* Saves @record under the default organization, expecting success. */
static void
server_save(
	ServerFixture	*fixture,
	gpointer	 record
){
	g_autoptr(GError) error = NULL;

	venture_entity_set_organization_id(VENTURE_ENTITY(record),
		venture_context_get_default_organization_id(fixture->context));

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(record), NULL, &error))
		g_error("save refused: %s", error->message);
}


/* The JSON body's items, by title. */
static GPtrArray *
json_titles(const gchar *body)
{
	g_autoptr(JsonNode) node = NULL;
	JsonArray *rows;
	GPtrArray *titles;
	guint i;

	node = venture_json_parse(body, NULL);
	g_assert_nonnull(node);
	rows = json_object_get_array_member(json_node_get_object(node), "items");
	titles = g_ptr_array_new_with_free_func(g_free);

	for (i = 0; i < json_array_get_length(rows); i++)
		g_ptr_array_add(titles, g_strdup(json_object_get_string_member(
			json_array_get_object_element(rows, i), "title")));

	return titles;
}

/*
 * The three doors are one answer: every title the API returns is on the
 * agenda page and in the feed; the month grid is marked data-no-sort;
 * "Only mine" narrows to the signed-in user's tickets; and an event's
 * page carries its run-sheet with running times and its preparation
 * count before the generic panels. What breaks: a page that computes its
 * own idea of the month, a grid the table script reorders by column, or
 * an event page that lists its agenda as a wall of names in id order.
 */
static void
test_http(
	ServerFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) event = NULL;
	g_autoptr(VentureEntity) item = NULL;
	g_autoptr(VentureEntity) later = NULL;
	g_autoptr(VentureEntity) mine = NULL;
	g_autoptr(VentureEntity) theirs = NULL;
	g_autoptr(VentureEntity) prep = NULL;
	g_autoptr(GPtrArray) titles = NULL;
	g_autoptr(GPtrArray) my_titles = NULL;
	g_autofree gchar *json = NULL;
	g_autofree gchar *page = NULL;
	g_autofree gchar *month = NULL;
	g_autofree gchar *feed = NULL;
	g_autofree gchar *mine_json = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *event_page = NULL;
	g_autofree gchar *bad = NULL;
	guint i;

	(void)user_data;

	event = VENTURE_ENTITY(venture_event_new());
	g_object_set(event, "name", "Quarterly review", "online-url", "https://meet.example.test/q", NULL);
	g_assert_true(venture_entity_set_field_from_string(event, "starts-at", "2026-03-09T09:00:00Z", NULL));
	g_assert_true(venture_entity_set_field_from_string(event, "ends-at", "2026-03-09T10:00:00Z", NULL));
	server_save(fixture, event);

	item = VENTURE_ENTITY(venture_event_agenda_item_new());
	g_object_set(item, "event-id", ID(event), "title", "Opening", "position", (gint64)1,
	             "minutes", (gint64)20, NULL);
	server_save(fixture, item);
	later = VENTURE_ENTITY(venture_event_agenda_item_new());
	g_object_set(later, "event-id", ID(event), "title", "Figures", "position", (gint64)2,
	             "minutes", (gint64)25, NULL);
	server_save(fixture, later);

	mine = VENTURE_ENTITY(venture_ticket_new());
	g_object_set(mine, "title", "Book the room", "assignee", "owner", "event-id", ID(event), NULL);
	g_assert_true(venture_entity_set_field_from_string(mine, "due-at", "2026-03-05", NULL));
	server_save(fixture, mine);
	theirs = VENTURE_ENTITY(venture_ticket_new());
	g_object_set(theirs, "title", "Print slides", "assignee", "someone", "event-id", ID(event), NULL);
	g_assert_true(venture_entity_set_field_from_string(theirs, "due-at", "2026-03-06", NULL));
	g_assert_true(venture_entity_set_field_from_string(theirs, "status", "done", NULL));
	server_save(fixture, theirs);

	/* --- One answer, three shapes --- */

	g_assert_cmpuint(server_request(fixture, "GET",
		"/api/v1/calendar?from=2026-03-01&to=2026-03-31&done=1", NULL, NULL, &json), ==, SOUP_STATUS_OK);
	titles = json_titles(json);
	g_assert_cmpuint(titles->len, ==, 3);
	g_assert_cmpuint(server_request(fixture, "GET",
		"/calendar?view=agenda&from=2026-03-01&to=2026-03-31&done=1", NULL, NULL, &page), ==, SOUP_STATUS_OK);
	g_assert_cmpuint(server_request(fixture, "GET",
		"/calendar.ics?from=2026-03-01&to=2026-03-31&done=1", NULL, NULL, &feed), ==, SOUP_STATUS_OK);

	for (i = 0; i < titles->len; i++)
	{
		g_autofree gchar *summary = g_strdup_printf("SUMMARY:%s\r\n", (gchar *)g_ptr_array_index(titles, i));

		ASSERT_CONTAINS(page, (const gchar *)g_ptr_array_index(titles, i));
		ASSERT_CONTAINS(feed, summary);
	}

	g_assert_cmpuint(server_request(fixture, "GET",
		"/calendar?view=month&date=2026-03-09", NULL, NULL, &month), ==, SOUP_STATUS_OK);
	ASSERT_CONTAINS(month, "<table class=\"cal-grid cal-month\" data-no-sort>");
	ASSERT_CONTAINS(month, "cal-item cal-kind-event");
	ASSERT_CONTAINS(month, "Quarterly review");
	/* An undone ticket is drawn, a done one only when asked. */
	ASSERT_CONTAINS(month, "Book the room");
	g_assert_null(strstr(month, "Print slides"));

	/* --- Only mine: the signed-in owner's tickets, not a colleague's --- */

	g_assert_cmpuint(server_request(fixture, "GET",
		"/api/v1/calendar?from=2026-03-01&to=2026-03-31&done=1&assignee=me&kind=ticket", NULL, NULL,
		&mine_json), ==, SOUP_STATUS_OK);
	my_titles = json_titles(mine_json);
	g_assert_cmpuint(my_titles->len, ==, 1);
	g_assert_cmpstr(g_ptr_array_index(my_titles, 0), ==, "Book the room");

	/* A narrowing nobody understands is refused, not ignored. */
	g_assert_cmpuint(server_request(fixture, "GET",
		"/api/v1/calendar?assignee=someone", NULL, NULL, &bad), ==, 422);

	/* --- The event's own block --- */

	path = g_strdup_printf("/e/event/%" G_GINT64_FORMAT, ID(event));
	g_assert_cmpuint(server_request(fixture, "GET", path, NULL, NULL, &event_page), ==, SOUP_STATUS_OK);
	{
		const gchar *block = strstr(event_page, "<div class=\"card event-agenda\">");
		const gchar *opening;
		const gchar *figures;

		g_assert_nonnull(block);
		ASSERT_CONTAINS(block, "45 min");
		opening = strstr(block, ">09:00<");
		figures = strstr(block, ">09:20<");
		g_assert_nonnull(opening);
		g_assert_nonnull(figures);
		g_assert_true(opening < figures);
		ASSERT_CONTAINS(event_page, "1 of 2 done");
		ASSERT_CONTAINS(event_page, "href=\"https://meet.example.test/q\">Join online</a>");
	}
}

/* ==========================================================================
 * The CLI
 * ========================================================================== */

typedef struct
{
	Fixture			 base;
	VentureWebServer	*server;
	gchar			*directory;
} CliFixture;

typedef struct
{
	gboolean	 done;
	GError		*error;
	gchar		*out;
	gchar		*err;
} CliResult;

static void
cli_fixture_set_up(
	CliFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	fixture->directory = g_dir_make_tmp("venture-planner-cli-XXXXXX", &error);
	g_assert_no_error(error);
	fixture_set_up(&fixture->base, user_data);
	g_object_set(fixture->base.config, "state-dir", fixture->directory,
	             "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
	             "security-require-auth", FALSE, NULL);
	fixture->server = venture_web_server_new(fixture->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(fixture->server, &error));
	g_assert_no_error(error);
}

static void
cli_fixture_tear_down(
	CliFixture	*fixture,
	gconstpointer	 user_data
){
	venture_web_server_stop(fixture->server);
	g_clear_object(&fixture->server);
	fixture_tear_down(&fixture->base, user_data);
	venture_test_remove_tree(fixture->directory);
	g_free(fixture->directory);
}

static void
cli_done(
	GObject		*source,
	GAsyncResult	*result,
	gpointer	 user_data
){
	CliResult *outcome = user_data;

	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
	                                     &outcome->out, &outcome->err, &outcome->error);
	outcome->done = TRUE;
}

/* Runs venturectl against the fixture's server; returns stdout. */
static gchar *
run_cli(
	CliFixture		*fixture,
	const gchar *const	*args,
	gboolean		 expect_success
){
	g_autoptr(GSubprocessLauncher) launcher = NULL;
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = NULL;
	g_autoptr(GError) error = NULL;
	CliResult result;
	guint i;

	memset(&result, 0, sizeof(result));
	launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	argv = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(argv, g_canonicalize_filename(VENTURE_TEST_BIN_DIR "/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server"));
	g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(fixture->server)));

	for (i = 0; NULL != args[i]; i++)
		g_ptr_array_add(argv, g_strdup(args[i]));

	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "planner-fixture", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &result);

	while (!result.done)
		g_main_context_iteration(NULL, TRUE);

	g_assert_no_error(result.error);

	if (g_subprocess_get_successful(process) != expect_success)
		g_error("venturectl: %s%s", result.out, result.err);

	g_free(result.err);

	return result.out;
}

/*
 * venturectl calendar show is the API's answer printed by day; -f json is
 * the answer itself; an option it does not know is refused by name. What
 * breaks: a convenience verb that drops a filter, or sends a misspelt one
 * the route ignores and answers a different question.
 */
static void
test_cli(
	CliFixture	*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureEntity) event = NULL;
	const gchar *const agenda[] = { "calendar", "show", "from=2026-03-01", "to=2026-03-31", NULL };
	const gchar *const tickets[] = { "calendar", "show", "from=2026-03-01", "to=2026-03-31", "kind=ticket", NULL };
	const gchar *const json[] = { "-f", "json", "calendar", "show", "from=2026-03-01", "to=2026-03-31", NULL };
	const gchar *const typo[] = { "calendar", "show", "kinds=ticket", NULL };
	g_autofree gchar *out = NULL;
	g_autofree gchar *only = NULL;
	g_autofree gchar *body = NULL;
	g_autofree gchar *refused = NULL;

	(void)user_data;

	event = event_new(&fixture->base, "Planning day", "2026-03-11T09:00:00Z", "2026-03-11T12:00:00Z");
	save(&fixture->base, event);
	ticket(&fixture->base, "File the report", "2026-03-12", NULL);

	out = run_cli(fixture, agenda, TRUE);
	ASSERT_CONTAINS(out, "2026-03-11");
	ASSERT_CONTAINS(out, "Planning day");
	ASSERT_CONTAINS(out, "File the report");

	only = run_cli(fixture, tickets, TRUE);
	ASSERT_CONTAINS(only, "File the report");
	g_assert_null(strstr(only, "Planning day"));

	body = run_cli(fixture, json, TRUE);
	ASSERT_CONTAINS(body, "\"items\"");
	ASSERT_CONTAINS(body, "\"record_type\"");

	refused = run_cli(fixture, typo, FALSE);
}

int
main(
	int	 argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/planner/event/rules", test_event_rules);
	ADD("/planner/attendee/rules", test_attendee_rules);
	ADD("/planner/agenda/running-times", test_agenda_running_times);
	ADD("/planner/prep-tickets", test_prep_tickets);
	ADD("/planner/calendar/sources", test_sources_in_and_out);
	ADD("/planner/calendar/spans", test_spans_overlap);
	ADD("/planner/calendar/kind-filter", test_kind_filter);
	ADD("/planner/calendar/venture-filter", test_venture_filter);
	ADD("/planner/calendar/module-off", test_module_off);
	ADD("/planner/calendar/row-bound", test_row_bound);
	ADD("/planner/calendar/recurrence", test_recurrence);
	ADD("/planner/calendar/ics", test_ics);
	ADD("/planner/calendar/view-range", test_view_range);
	ADD("/planner/calendar/money", test_money_source);
	ADD("/planner/calendar/registry", test_source_registry);
	g_test_add("/planner/cli", CliFixture, NULL, cli_fixture_set_up,
	           test_cli, cli_fixture_tear_down);
	g_test_add("/planner/http", ServerFixture, NULL, server_fixture_set_up,
	           test_http, server_fixture_tear_down);

#undef ADD

	return g_test_run();
}
