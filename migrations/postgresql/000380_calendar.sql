-- Calendar sync tables come from the calendar module field tables:
-- calendar_accounts, calendar_events and booking_pages. Nothing is
-- rewritten. Verify the three arrived together, or are all absent when
-- the module is disabled, and that the two keys a rerun rests on exist:
-- (organization, uid_key) on calendar_events is what makes a second
-- sweep a no-op, and (organization, slug) on booking_pages is what makes
-- /book/<slug> answer one page.
CREATE TEMP TABLE venture_calendar_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_calendar_upgrade_guard
SELECT CASE WHEN (SELECT COUNT(*) FROM information_schema.tables
  WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
  AND table_name IN ('calendar_accounts', 'calendar_events', 'booking_pages')) NOT IN (0, 3) THEN 0 ELSE 1 END;
INSERT INTO venture_calendar_upgrade_guard
SELECT CASE WHEN to_regclass('calendar_events') IS NOT NULL
 AND to_regclass('uq_calendar_events_organization_uid_key') IS NULL THEN 0 ELSE 1 END;
INSERT INTO venture_calendar_upgrade_guard
SELECT CASE WHEN to_regclass('booking_pages') IS NOT NULL
 AND to_regclass('uq_booking_pages_organization_slug') IS NULL THEN 0 ELSE 1 END;
DROP TABLE venture_calendar_upgrade_guard;
