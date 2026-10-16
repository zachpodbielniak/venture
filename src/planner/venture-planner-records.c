/*
 * venture-planner-records.c - Events, their run-sheets and who is coming
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "venture.h"

/* ==========================================================================
 * Enumerations
 *
 * Registered here rather than in venture-enums.c, beside the only types
 * that use them, as the activities module does.
 * ========================================================================== */

#define PLANNER_ENUM(func, name, ...)                                         \
	GType                                                                 \
	func(void)                                                            \
	{                                                                     \
		static gsize type_id = 0;                                     \
                                                                              \
		if (g_once_init_enter(&type_id))                              \
		{                                                             \
			static const GEnumValue values[] = {                  \
				__VA_ARGS__,                                  \
				{ 0, NULL, NULL }                             \
			};                                                    \
			GType id;                                             \
                                                                              \
			id = g_enum_register_static(name, values);            \
			g_once_init_leave(&type_id, id);                      \
		}                                                             \
                                                                              \
		return type_id;                                               \
	}

#define PV(sym, nick) { sym, #sym, nick }

PLANNER_ENUM(venture_event_kind_get_type, "VentureEventKind",
	PV(VENTURE_EVENT_KIND_MEETING, "meeting"),
	PV(VENTURE_EVENT_KIND_APPOINTMENT, "appointment"),
	PV(VENTURE_EVENT_KIND_DEADLINE, "deadline"),
	PV(VENTURE_EVENT_KIND_CONFERENCE, "conference"),
	PV(VENTURE_EVENT_KIND_TRIP, "trip"),
	PV(VENTURE_EVENT_KIND_LAUNCH, "launch"),
	PV(VENTURE_EVENT_KIND_PERSONAL, "personal"),
	PV(VENTURE_EVENT_KIND_OTHER, "other"))

PLANNER_ENUM(venture_event_status_get_type, "VentureEventStatus",
	PV(VENTURE_EVENT_STATUS_CONFIRMED, "confirmed"),
	PV(VENTURE_EVENT_STATUS_TENTATIVE, "tentative"),
	PV(VENTURE_EVENT_STATUS_CANCELLED, "cancelled"))

PLANNER_ENUM(venture_event_recurrence_get_type, "VentureEventRecurrence",
	PV(VENTURE_EVENT_RECURRENCE_NONE, "none"),
	PV(VENTURE_EVENT_RECURRENCE_DAILY, "daily"),
	PV(VENTURE_EVENT_RECURRENCE_WEEKLY, "weekly"),
	PV(VENTURE_EVENT_RECURRENCE_MONTHLY, "monthly"),
	PV(VENTURE_EVENT_RECURRENCE_YEARLY, "yearly"))

PLANNER_ENUM(venture_attendee_role_get_type, "VentureAttendeeRole",
	PV(VENTURE_ATTENDEE_ROLE_REQUIRED, "required"),
	PV(VENTURE_ATTENDEE_ROLE_OPTIONAL, "optional"),
	PV(VENTURE_ATTENDEE_ROLE_ORGANIZER, "organizer"))

PLANNER_ENUM(venture_attendee_rsvp_get_type, "VentureAttendeeRsvp",
	PV(VENTURE_ATTENDEE_RSVP_PENDING, "pending"),
	PV(VENTURE_ATTENDEE_RSVP_YES, "yes"),
	PV(VENTURE_ATTENDEE_RSVP_NO, "no"),
	PV(VENTURE_ATTENDEE_RSVP_MAYBE, "maybe"))

/* ==========================================================================
 * Events
 *
 * Something that happens at a time. `starts-at` is required and `ends-at`
 * is not: a deadline has no length. An all-day event's times are calendar
 * dates -- midnight UTC, the rule every date in VENTURE follows -- and its
 * end is exclusive, so a one-day event runs from its day to the next; the
 * validator normalises both (venture-planner.c).
 *
 * `location` is the short "where" a person reads on the calendar; the
 * full postal `address` is long text beside it. Neither is the `location`
 * record type, which is an inventory place with no address. `online-url`
 * is shown as a link only when it is a web address.
 *
 * A recurring event is one record; its occurrences are computed when a
 * calendar is asked for, bounded, and never stored.
 * ========================================================================== */

static const VentureFieldDecl venture_event_fields[] = {
	VENTURE_FIELD_NAME("name", "Name", "What it is: Board meeting, Spring fair"),
	VENTURE_FIELD_ENUM("kind", "Kind", "Meeting, appointment, deadline, conference, trip, launch, personal or other",
	                   venture_event_kind_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_ENUM("status", "Status", "Confirmed unless you say tentative or cancelled",
	                   venture_event_status_get_type, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("starts-at", "Starts", "When it begins; a date for an all-day event",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("ends-at", "Ends", "Optional: when it is over; for an all-day event its last day",
	              VENTURE_FIELD_KIND_DATETIME, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD("location", "Where", "Optional: the place in a few words -- Room 4, Berlin, the cafe",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("venture-id", "Venture", "Optional: the venture it is for",
	                  "venture", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("all-day", "All day", "Takes whole days; its times are dates",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("address", "Address", "Optional: the full address"),
	VENTURE_FIELD("online-url", "Online link", "Optional: the call or stream address",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("description", "Description", "What it is for"),
	VENTURE_FIELD_REF("organizer-user-id", "Organizer", "Optional: the user running it",
	                  "user", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("goal-id", "Goal", "Optional: the goal it serves",
	                  "goal", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("milestone-id", "Milestone", "Optional: the milestone it belongs to",
	                  "milestone", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("contact-id", "Contact", "Optional: who it is with",
	                  "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("company-id", "Company", "Optional: which company it is with",
	                  "company", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("recurrence", "Repeats", "None, daily, weekly, monthly or yearly",
	                   venture_event_recurrence_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("recurs-until", "Repeats until", "Optional: the last day it may repeat on",
	              VENTURE_FIELD_KIND_DATE, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("tags", "Tags", "Comma separated",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureEvent, venture_event, venture_event_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Event", NULL);)

/* ==========================================================================
 * Agenda items
 *
 * One line of an event's run-sheet: what happens, for how long, who
 * leads it. `position` orders them; each item's start is the event's start
 * plus every earlier item's minutes, computed by
 * venture_planner_agenda() and never stored -- a stored start would go
 * stale the moment an earlier item was lengthened.
 *
 * `done` is named for completion so the zero value, not done, is the safe
 * one: an item nobody ticked is still ahead.
 * ========================================================================== */

static const VentureFieldDecl venture_event_agenda_item_fields[] = {
	VENTURE_FIELD_REF("event-id", "Event", "The event this item belongs to",
	                  "event", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("position", "Order", "Where it comes in the run-sheet; lowest first",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_INDEXED),
	VENTURE_FIELD_NAME("title", "Title", "What happens: Welcome, Q3 numbers, Questions"),
	VENTURE_FIELD("minutes", "Minutes", "How long it takes; 0 for a moment",
	              VENTURE_FIELD_KIND_INTEGER, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD("presenter", "Presenter", "Optional: who leads it",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("done", "Done", "Covered",
	              VENTURE_FIELD_KIND_BOOLEAN, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_TEXT("notes", "Notes", NULL)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureEventAgendaItem, venture_event_agenda_item,
	venture_event_agenda_item_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Agenda item", NULL);)

/* ==========================================================================
 * Attendees
 *
 * Somebody expected at an event: a contact, a user, or just a name or an
 * address for a person who is neither yet. At least one of the four is
 * required, or the row names nobody. The role and the answer start at
 * their safe zero values: required, and pending.
 * ========================================================================== */

static const VentureFieldDecl venture_event_attendee_fields[] = {
	VENTURE_FIELD_REF("event-id", "Event", "The event they are invited to",
	                  "event", VENTURE_COLUMN_FLAG_NOT_NULL),
	VENTURE_FIELD("name", "Name", "Optional when a contact or user is named",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD("email", "Email", "Optional: where to reach them",
	              VENTURE_FIELD_KIND_STRING, VENTURE_COLUMN_FLAG_SEARCHABLE),
	VENTURE_FIELD_REF("contact-id", "Contact", "Optional: the contact this is",
	                  "contact", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_REF("user-id", "User", "Optional: the user this is",
	                  "user", VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("role", "Role", "Required, optional or organizer",
	                   venture_attendee_role_get_type, VENTURE_COLUMN_FLAG_NONE),
	VENTURE_FIELD_ENUM("rsvp", "Answer", "Pending until they say yes, no or maybe",
	                   venture_attendee_rsvp_get_type, VENTURE_COLUMN_FLAG_INDEXED)
};

VENTURE_DEFINE_ENTITY_WITH_CODE(VentureEventAttendee, venture_event_attendee,
	venture_event_attendee_fields,
	venture_entity_class_set_labels(VENTURE_ENTITY_CLASS(klass), "Attendee", NULL);)
