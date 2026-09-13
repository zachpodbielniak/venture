-- Planned activities have no history to backfill from past interactions.
-- Notification delivery is core and exists with activities switched off.
-- Keep owner/target reminder lookups scoped to their organization.
CREATE INDEX IF NOT EXISTS activities_notification_target
ON notifications (organization_id, target_type, target_id, user_id);
