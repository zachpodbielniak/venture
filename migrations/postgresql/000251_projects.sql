-- Client project costing records come from field tables. Complete family or none.
CREATE TEMP TABLE venture_projects_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 5)));
INSERT INTO venture_projects_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('client_projects', 'project_rates', 'project_times', 'project_costs', 'project_billings');
DROP TABLE venture_projects_upgrade_guard;
