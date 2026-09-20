-- Additive metadata owns fields and the organization-scoped external-key
-- constraint. Old completions remain untouched and have unknown results.
CREATE TEMP TABLE venture_call_upgrade_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_call_upgrade_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'activities')
 OR (SELECT COUNT(*) FROM pragma_table_info('activities') WHERE name IN
 ('lead_id', 'call_occurred_at', 'call_duration', 'call_outcome', 'call_external_key',
  'call_request_hash', 'call_interaction_id', 'call_followup_id')) = 8 THEN 1 ELSE 0 END;
DROP TABLE venture_call_upgrade_guard;
