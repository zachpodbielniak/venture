-- requires-table: documents
-- Existing documents carry no warranty and remind nobody.
UPDATE documents SET warranty_reminder_user_id = 0 WHERE warranty_reminder_user_id IS NULL;
