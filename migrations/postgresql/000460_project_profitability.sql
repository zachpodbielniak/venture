-- Existing approved time has unknown cost. Never reconstruct historical
-- labour cost from today's editable rate. Metadata adds nullable evidence.
CREATE TEMP TABLE venture_project_cost_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_project_cost_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_name = 'project_times')
 OR (SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = current_schema()
 AND table_name = 'project_times' AND column_name IN ('actual_cost_amount', 'actual_cost_currency', 'actual_cost_exponent')) = 3 THEN 1 ELSE 0 END;
DROP TABLE venture_project_cost_guard;
