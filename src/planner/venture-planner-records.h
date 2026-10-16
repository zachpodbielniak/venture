/*
 * venture-planner-records.h - Events, their run-sheets and who is coming
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The planner module's three record types. An event is something that
 * happens at a time -- a meeting, a conference, a launch, a trip; its
 * agenda items are the ordered run-sheet of it; its attendees are who is
 * expected and what they answered. All three are field tables and nothing
 * else: the rules that span rows live in venture-planner.c as save
 * validators, so every writer obeys them.
 */

#ifndef VENTURE_PLANNER_RECORDS_H
#define VENTURE_PLANNER_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

/**
 * VentureEventKind:
 * @VENTURE_EVENT_KIND_MEETING: a meeting; the zero value, because most
 *   things anybody puts in a diary are one
 * @VENTURE_EVENT_KIND_APPOINTMENT: an appointment kept somewhere else
 * @VENTURE_EVENT_KIND_DEADLINE: a moment something is due, with no length
 * @VENTURE_EVENT_KIND_CONFERENCE: a conference, fair or convention
 * @VENTURE_EVENT_KIND_TRIP: travel, usually several days
 * @VENTURE_EVENT_KIND_LAUNCH: a launch or release day
 * @VENTURE_EVENT_KIND_PERSONAL: a personal commitment
 * @VENTURE_EVENT_KIND_OTHER: anything else
 *
 * What shape of event it is. It decides nothing but the label and the
 * colour on the calendar.
 */
typedef enum
{
	VENTURE_EVENT_KIND_MEETING = 0,
	VENTURE_EVENT_KIND_APPOINTMENT,
	VENTURE_EVENT_KIND_DEADLINE,
	VENTURE_EVENT_KIND_CONFERENCE,
	VENTURE_EVENT_KIND_TRIP,
	VENTURE_EVENT_KIND_LAUNCH,
	VENTURE_EVENT_KIND_PERSONAL,
	VENTURE_EVENT_KIND_OTHER
} VentureEventKind;

/**
 * venture_event_kind_get_type:
 *
 * Returns: the #VentureEventKind enumeration type
 */
GType venture_event_kind_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_EVENT_KIND (venture_event_kind_get_type())

/**
 * VentureEventStatus:
 * @VENTURE_EVENT_STATUS_CONFIRMED: going ahead; the zero value, because an
 *   event somebody wrote down without saying otherwise is one they expect
 *   to happen, and a calendar that hid it as "tentative" would be the
 *   surprising answer
 * @VENTURE_EVENT_STATUS_TENTATIVE: pencilled in, not settled
 * @VENTURE_EVENT_STATUS_CANCELLED: called off; kept for the record and
 *   left off the calendar unless closed items are asked for
 *
 * Whether an event is happening.
 */
typedef enum
{
	VENTURE_EVENT_STATUS_CONFIRMED = 0,
	VENTURE_EVENT_STATUS_TENTATIVE,
	VENTURE_EVENT_STATUS_CANCELLED
} VentureEventStatus;

/**
 * venture_event_status_get_type:
 *
 * Returns: the #VentureEventStatus enumeration type
 */
GType venture_event_status_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_EVENT_STATUS (venture_event_status_get_type())

/**
 * VentureEventRecurrence:
 * @VENTURE_EVENT_RECURRENCE_NONE: happens once; the zero value, so an event
 *   saved without saying is not silently repeated forever
 * @VENTURE_EVENT_RECURRENCE_DAILY: every day
 * @VENTURE_EVENT_RECURRENCE_WEEKLY: every week on the same weekday
 * @VENTURE_EVENT_RECURRENCE_MONTHLY: every month on the same day, clamped
 *   to the month's last day
 * @VENTURE_EVENT_RECURRENCE_YEARLY: every year on the same date
 *
 * How an event repeats. The occurrences are computed when a calendar is
 * asked for, never stored.
 */
typedef enum
{
	VENTURE_EVENT_RECURRENCE_NONE = 0,
	VENTURE_EVENT_RECURRENCE_DAILY,
	VENTURE_EVENT_RECURRENCE_WEEKLY,
	VENTURE_EVENT_RECURRENCE_MONTHLY,
	VENTURE_EVENT_RECURRENCE_YEARLY
} VentureEventRecurrence;

/**
 * venture_event_recurrence_get_type:
 *
 * Returns: the #VentureEventRecurrence enumeration type
 */
GType venture_event_recurrence_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_EVENT_RECURRENCE (venture_event_recurrence_get_type())

/**
 * VentureAttendeeRole:
 * @VENTURE_ATTENDEE_ROLE_REQUIRED: expected to be there; the zero value
 * @VENTURE_ATTENDEE_ROLE_OPTIONAL: welcome, not needed
 * @VENTURE_ATTENDEE_ROLE_ORGANIZER: running it
 *
 * What an attendee is to the event.
 */
typedef enum
{
	VENTURE_ATTENDEE_ROLE_REQUIRED = 0,
	VENTURE_ATTENDEE_ROLE_OPTIONAL,
	VENTURE_ATTENDEE_ROLE_ORGANIZER
} VentureAttendeeRole;

/**
 * venture_attendee_role_get_type:
 *
 * Returns: the #VentureAttendeeRole enumeration type
 */
GType venture_attendee_role_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_ATTENDEE_ROLE (venture_attendee_role_get_type())

/**
 * VentureAttendeeRsvp:
 * @VENTURE_ATTENDEE_RSVP_PENDING: not answered yet; the zero value, so
 *   nobody is counted as coming because a form left the box alone
 * @VENTURE_ATTENDEE_RSVP_YES: coming
 * @VENTURE_ATTENDEE_RSVP_NO: not coming
 * @VENTURE_ATTENDEE_RSVP_MAYBE: might come
 *
 * What an attendee answered.
 */
typedef enum
{
	VENTURE_ATTENDEE_RSVP_PENDING = 0,
	VENTURE_ATTENDEE_RSVP_YES,
	VENTURE_ATTENDEE_RSVP_NO,
	VENTURE_ATTENDEE_RSVP_MAYBE
} VentureAttendeeRsvp;

/**
 * venture_attendee_rsvp_get_type:
 *
 * Returns: the #VentureAttendeeRsvp enumeration type
 */
GType venture_attendee_rsvp_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_ATTENDEE_RSVP (venture_attendee_rsvp_get_type())

#define VENTURE_TYPE_EVENT (venture_event_get_type())
VENTURE_DECLARE_ENTITY(VentureEvent, venture_event, EVENT)

#define VENTURE_TYPE_EVENT_AGENDA_ITEM (venture_event_agenda_item_get_type())
VENTURE_DECLARE_ENTITY(VentureEventAgendaItem, venture_event_agenda_item, EVENT_AGENDA_ITEM)

#define VENTURE_TYPE_EVENT_ATTENDEE (venture_event_attendee_get_type())
VENTURE_DECLARE_ENTITY(VentureEventAttendee, venture_event_attendee, EVENT_ATTENDEE)

G_END_DECLS

#endif /* VENTURE_PLANNER_RECORDS_H */
