-- requires-table: events
-- Additive: the planner module's events, their agenda items and their
-- attendees. New tables, created by reconciliation from their field
-- tables; nothing existed before them, so nothing is backfilled. This
-- verifies the columns the calendar and the event page read.
SELECT name, kind, status, starts_at, ends_at, all_day, location, address, online_url, description, notes, organizer_user_id, venture_id, goal_id, milestone_id, contact_id, company_id, recurrence, recurs_until, tags FROM events WHERE 1 = 0;
SELECT event_id, position, title, minutes, presenter, notes, done FROM event_agenda_items WHERE 1 = 0;
SELECT event_id, contact_id, user_id, name, email, role, rsvp FROM event_attendees WHERE 1 = 0;
