-- Existing approved time has unknown cost. Never reconstruct historical
-- labour cost from today's editable rate. Metadata adds nullable evidence.
CREATE TEMP TABLE venture_project_cost_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_project_cost_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = 'project_times')
 OR (SELECT COUNT(*) FROM pragma_table_info('project_times') WHERE name IN ('actual_cost_amount', 'actual_cost_currency', 'actual_cost_exponent')) = 3 THEN 1 ELSE 0 END;
DROP TABLE venture_project_cost_guard;
