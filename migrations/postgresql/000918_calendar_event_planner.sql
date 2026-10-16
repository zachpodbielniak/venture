-- requires-table: calendar_events
-- Additive: the planner event a CalDAV copy was pushed from. Every
-- existing link mirrors an activity and names no event.
SELECT planner_event_id FROM calendar_events WHERE 1 = 0;
