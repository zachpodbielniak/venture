-- Recurring documents, collections and batch templates.
-- Field tables create the records and organization-scoped identities.
-- Disabled or never-enabled modules are valid; no history is inferred.
CREATE TEMP TABLE venture_recurring_upgrade_guard (table_count INTEGER CHECK (table_count IN (0, 7)));
INSERT INTO venture_recurring_upgrade_guard SELECT COUNT(*) FROM information_schema.tables
 WHERE table_schema = current_schema() AND table_type = 'BASE TABLE'
 AND table_name IN ('recurring_schedules', 'recurring_occurrences',
  'collection_policies', 'collection_steps', 'collection_cases', 'collection_notices', 'financial_batches');
DROP TABLE venture_recurring_upgrade_guard;
CREATE TEMP TABLE venture_recurring_identity_guard (valid INTEGER NOT NULL CHECK (valid = 1));
INSERT INTO venture_recurring_identity_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_type = 'BASE TABLE' AND table_name = 'recurring_occurrences')
 OR EXISTS (SELECT 1 FROM pg_indexes WHERE schemaname = current_schema() AND indexname = 'uq_recurring_occurrences_organization_occurrence_key') THEN 1 ELSE 0 END;
INSERT INTO venture_recurring_identity_guard
SELECT CASE WHEN NOT EXISTS (SELECT 1 FROM information_schema.tables WHERE table_schema = current_schema() AND table_type = 'BASE TABLE' AND table_name = 'collection_notices')
 OR EXISTS (SELECT 1 FROM pg_indexes WHERE schemaname = current_schema() AND indexname = 'uq_collection_notices_organization_occurrence_key') THEN 1 ELSE 0 END;
DROP TABLE venture_recurring_identity_guard;
