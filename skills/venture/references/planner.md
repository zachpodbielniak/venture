# The planner: events, one calendar, the roadmap

Read this for meetings, appointments, conferences, trips and launches with
their run-sheets, attendees, reminders and budgets; for "what is happening
when" across every module (page, JSON, `.ics`, org-agenda); for the
roadmap of goals, milestones and epics; and for the `agenda` widget.
Source: `docs/planner.org`.

Refusals are validation errors (**exit 8**, HTTP 422) unless noted.

## Events (module `planner`, requires only `core`)

Check `describe event`, `describe event_agenda_item`, `describe event_attendee`.

- `event`: **name**, `kind` [meeting|appointment|deadline|conference|trip|launch|personal|other],
  `status` [confirmed|tentative|cancelled] (confirmed is the default: an
  event written down is expected to happen), **starts_at**, `ends_at`,
  `all_day`, `location` (a few words; *not* the `location` record type),
  `address`, `online_url`, `description`, `notes`, `organizer_user_id`,
  `venture_id`, `goal_id`, `milestone_id`, `contact_id`, `company_id`,
  `recurrence` [none|daily|weekly|monthly|yearly], `recurs_until`, `tags`,
  `remind_minutes` (0 = none, at most 40320), `budget` (money, not
  negative). `reminded_for` is the sweep's stamp; never write it.
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
- **Budget**: an `expense`'s `event_id` (same organization, checked when
  written) counts it against the event's `budget`. The page shows spend
  per currency and what is left *in the budget's currency only* -- other
  currencies are listed, never converted. Over budget warns, never refuses.
- **Reminders**: `remind_minutes` tells the organizer and every attendee
  who is a user and did not answer `no` -- each once per occurrence, only
  if they may read the event -- with an inbox notification of kind
  `reminder`. The sweep is lazy (inbox, board, calendar page) or
  `POST /api/v1/calendar/reminders` / `venturectl calendar remind`
  (editor; prints `{"reminded": N}`). An occurrence already under way is
  skipped; a cancelled event never reminds.

```bash
venturectl create event name="Board meeting" starts_at=2026-03-09T09:00:00Z ends_at=2026-03-09T10:00:00Z venture_id=2
venturectl create event name="Spring fair" kind=trip all_day=true starts_at=2026-04-10 ends_at=2026-04-12 location=Berlin
venturectl create event name="Standup" starts_at=2026-03-02T09:00:00Z ends_at=2026-03-02T09:15:00Z recurrence=weekly recurs_until=2026-06-30
venturectl create event_agenda_item event_id=4 title=Opening position=1 minutes=20 presenter=Ada
venturectl create event_attendee event_id=4 contact_id=12 rsvp=yes
venturectl create ticket title="Book the room" event_id=4 due_at=2026-03-05 assignee=ada
venturectl update event 4 remind_minutes=30 budget="500.00 USD"
venturectl create expense description="Venue deposit" amount="120.00 USD" occurred_at=2026-03-02 event_id=4
venturectl calendar remind
```

## The calendar

`GET /calendar` (page), `GET /api/v1/calendar` (JSON), `GET /calendar.ics`
(feed, viewer role -- subscribe with a token), `GET /calendar.org`
(org-agenda file, viewer role). One handler, one answer.

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
venturectl calendar org from=2026-03-01 to=2026-04-30 > ~/notes/venture-calendar.org
curl -H "Authorization: Bearer $VENTURE_TOKEN" "$VENTURE_SERVER/calendar.ics?from=2026-03-01&to=2026-06-30"
```

`calendar show` is a GET of `/api/v1/calendar` with every `k=v` in the
query string, printed as an agenda by day; `-f json|yaml` prints the
server's answer. `calendar org` takes the same options and prints
`/calendar.org` untouched.

**The org file**: a due date (ticket, activity, milestone, goal, deal,
money) is `* TODO`/`* DONE` with `DEADLINE: <...>`; an event, sprint or
CalDAV item is a heading with an active timestamp. Times are in the
configured zone; all-day ranges are `<a>--<b>` with the inclusive last day.
Each heading's drawer has `:ID:` (stable), `:VENTURE_TYPE:`,
`:VENTURE_RECORD:`, `:VENTURE_STATUS:`, `:VENTURE_URL:` (absolute with
`server.base_url`). Notes are `# note:` lines at the top. It is read-only:
closing a TODO in org changes nothing in VENTURE.

## The roadmap

`GET /roadmap` (page), `GET /api/v1/roadmap` (viewer), `venturectl
roadmap [goal_id=] [venture_id=] [from=] [to=] [done=1]`. Rows in drawing
order: each goal, its milestones each followed by their epics, the goal's
epics in none of its milestones; then milestones with no goal (with
epics); then epics in neither. Each record once. JSON `{from, to, notes,
rows}`; a row has `type` (goal|milestone|epic), `record_type`, `id`,
`title`, `depth`, `start` (created), `end` (due), `status`, `done`,
`overdue`, `work_done`, `work_total`, `url`. Progress is the record's own
page's figure. Default window: first of last month for seven months; at
most 1096 days. More than 500 goals, milestones or epics: refused --
narrow by `goal_id` or `venture_id`. A `goal_id` not in the organization
is NOT_FOUND. A finished goal (without `done=1`) hides its milestones and
epics too.

## The `agenda` widget

Dashboard widget kind `agenda` ("Coming up", module planner): the calendar
from today for `options.days` (14; 1-90), `options.kinds` (list or comma
string), `options.mine` (true: the viewer's own), at most `limit` (10).
Not the `upcoming` kind, which reads one type's date field.

```bash
venturectl create dashboard_widget dashboard_id=4 kind=agenda options='{"days":7,"kinds":["event","ticket"]}'
```

## CalDAV push

A shared `calendar_account` pushes its owner's planner events (organizer
= the user named by the account's `owner`) one way, UID
`<uuid>@venture-event`, with LOCATION, RRULE, all-day DATE values and a
VALARM from `remind_minutes`. Edits on the phone are not read back (the
next change here replaces them); a copy deleted on the server stays gone
until the event changes here; cancelling here pushes STATUS:CANCELLED.
The link is a `calendar_event` with `planner_event_id`.

## Traps

- Dates change on the record (`update event|ticket|deal ...`), never on
  the calendar.
- A repeating event is one record: editing it moves every occurrence;
  there is no per-occurrence exception -- make a separate event.
- An all-day `ends_at` is inclusive; a CalDAV feed's is exclusive. Do not
  "fix" one into the other.
- The planner off: `/calendar`, `/roadmap` are 404 and the API refuses;
  the records' generic routes hide the types like any module's.
- `/calendar.org` and `/calendar.ics` do not run the reminder sweep; a
  client polling on a timer is not a person looking. Use
  `calendar remind` from cron for reminders with nobody signed in.
- A monthly event on the 29th-31st: VENTURE clamps to the month's last
  day, the pushed RRULE skips short months.
