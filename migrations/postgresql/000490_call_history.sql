-- Metadata adds the same fields and scoped external-key index on each backend.
CREATE TEMP TABLE venture_call_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_call_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_name = 'activities')
 OR (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema()
 AND table_name = 'activities' AND column_name IN
 ('lead_id', 'call_occurred_at', 'call_duration', 'call_outcome', 'call_external_key',
  'call_request_hash', 'call_interaction_id', 'call_followup_id')) = 8 THEN 1 ELSE 0 END;
DROP TABLE venture_call_upgrade_guard;
