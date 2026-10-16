# The planner: events and one calendar

Read this for meetings, appointments, conferences, trips and launches with
their run-sheets and attendees, and for "what is happening when" across
every module. Source: `docs/planner.org`.

Refusals are validation errors (**exit 8**, HTTP 422) unless noted.

## Events (module `planner`, requires only `core`)

Check `describe event`, `describe event_agenda_item`, `describe event_attendee`.

- `event`: **name**, `kind` [meeting|appointment|deadline|conference|trip|launch|personal|other],
  `status` [confirmed|tentative|cancelled] (confirmed is the default: an
  event written down is expected to happen), **starts_at**, `ends_at`,
  `all_day`, `location` (a few words; *not* the `location` record type),
  `address`, `online_url`, `description`, `notes`, `organizer_user_id`,
  `venture_id`, `goal_id`, `milestone_id`, `contact_id`, `company_id`,
  `recurrence` [none|daily|weekly|monthly|yearly], `recurs_until`, `tags`.
- Refused: no start; a timed end before the start (equal is a moment);
  `recurs_until` on an event that does not repeat or before its first day;
  a reference into another organization.
- **All day**: both times become dates (midnight UTC on the day in the
  configured zone) and **`ends_at` is the last day, inclusive** -- "1 to 3
  March" is `starts_at=2026-03-01 ends_at=2026-03-03 all_day=true`. Empty
  end = one day. The feed writes the exclusive end itself.
- `event_agenda_item`: **event_id**, `position` (lowest first), **title**,
  `minutes` (>= 0), `presenter`, `notes`, `done`. Each item's start is the
  event's start plus the minutes before it -- computed on every read, never
  stored, so editing one item moves the rest. The event page warns when
  the sheet runs past the end.
- `event_attendee`: **event_id** and at least one of `contact_id`,
  `user_id`, `name`, `email` (an email needs an `@`); `role`
  [required|optional|organizer], `rsvp` [pending|yes|no|maybe].
- **Preparation is tickets**: a ticket's `event_id` names the event it
  prepares. The event page lists them as "N of M done"; cancelled ones
  count as neither.

```bash
venturectl create event name="Board meeting" starts_at=2026-03-09T09:00:00Z ends_at=2026-03-09T10:00:00Z venture_id=2
venturectl create event name="Spring fair" kind=trip all_day=true starts_at=2026-04-10 ends_at=2026-04-12 location=Berlin
venturectl create event name="Standup" starts_at=2026-03-02T09:00:00Z ends_at=2026-03-02T09:15:00Z recurrence=weekly recurs_until=2026-06-30
venturectl create event_agenda_item event_id=4 title=Opening position=1 minutes=20 presenter=Ada
venturectl create event_attendee event_id=4 contact_id=12 rsvp=yes
venturectl create ticket title="Book the room" event_id=4 due_at=2026-03-05 assignee=ada
```

## The calendar

`GET /calendar` (page), `GET /api/v1/calendar` (JSON), `GET /calendar.ics`
(feed, viewer role -- subscribe with a token). One handler, one answer.

| Parameter | Meaning |
|---|---|
| `view` | `month` (default), `week` (Monday first), `agenda` (30 days) |
| `date` | a day in the span; default today |
| `from`, `to` | explicit span, **both inclusive**; at most 367 days; `to` needs `from` |
| `kind` | repeat it or comma-list: `event`, `activity`, `ticket`, `milestone`, `goal`, `sprint`, `deal`, `money`, `caldav` (plus any a plugin registers); an unknown kind is refused |
| `venture_id` | one venture's records |
| `assignee` | only `me` (tickets/activities by your name, deals by owner, events you organize or attend); a token with no user is refused |
| `done` | `1`: also finished items (done tickets, completed milestones, achieved goals, won deals, cancelled events) |
| `organization_id` | the API's organization (pages use the sidebar pick) |

JSON: `{from, to, view, venture_id, include_done, assignee, kinds, notes,
count, items}`; an item has `kind`, `subkind`, `title`, `detail`, `start`,
`end` (exclusive), `all_day`, `day`, `record_type`, `record_id`, `url`,
`status`, `overdue`, `uid`.

**Read `notes`.** A source past the row bound (20 000) is *refused* --
no items, a note -- never truncated; a source that cannot answer a filter
(activities have no venture; milestones have no assignee) is left out with
a note; a kind whose module is off says so when named. An empty kind is
therefore not proof of nothing due.

What each source shows: events (not cancelled; repeating ones expanded,
one UID per occurrence), planned task/call/meeting/followup activities
(never email), open tickets by `due_at`, open milestones and goals by
`due_on`, sprints as spans, open deals by `expected_close_at`, the money
calendar's items (its own expander; overdue carried to today), and synced
CalDAV events that did not become an activity.

```bash
venturectl calendar show from=2026-03-01 to=2026-03-31
venturectl calendar show view=week date=2026-03-09 kind=event,ticket assignee=me
venturectl calendar show from=2026-03-01 to=2026-03-31 venture_id=2 done=1 -f json
curl -H "Authorization: Bearer $VENTURE_TOKEN" "$VENTURE_SERVER/calendar.ics?from=2026-03-01&to=2026-06-30"
```

`calendar show` is a GET of `/api/v1/calendar` with every `k=v` in the
query string, printed as an agenda by day; `-f json|yaml` prints the
server's answer.

## Traps

- Dates change on the record (`update event|ticket|deal ...`), never on
  the calendar.
- A repeating event is one record: editing it moves every occurrence;
  there is no per-occurrence exception -- make a separate event.
- An all-day `ends_at` is inclusive; a CalDAV feed's is exclusive. Do not
  "fix" one into the other.
- The planner off: `/calendar` is 404 and the API refuses; the records'
  generic routes hide the types like any module's.
