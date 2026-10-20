-- requires-table: recurring_schedules
-- Existing schedules retain their accounting behavior, without opt-in reminders.
UPDATE recurring_schedules SET subscription = FALSE WHERE subscription IS NULL;
UPDATE recurring_schedules SET projection_only = FALSE WHERE projection_only IS NULL;
UPDATE recurring_schedules SET reminder_days = 0 WHERE reminder_days IS NULL;
